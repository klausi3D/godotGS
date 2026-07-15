# Gaussian ProjectSettings Contract

This document defines the first-wave inventory contract for Gaussian
`ProjectSettings` keys. It is intentionally test-only/documentation-only: it
does not remove public settings, enable dormant renderer paths, or change
runtime behavior.

## Source Of Truth

The inventory lives in
`modules/gaussian_splatting/config/project_settings_manifest.json`.

Each exact `rendering/gaussian_splatting/*` key referenced by Gaussian
production C++ source must be listed in the manifest. Family defaults classify
common prefixes, and per-key entries provide the effective-state field, coverage
status, and notes for known gaps or cleanup candidates.

Required resolved fields:

| Field | Meaning |
| --- | --- |
| `owner` | The subsystem that owns registration, loading, or effective behavior. |
| `scope` | Runtime, diagnostic, debug, editor, import, migration, internal, or compatibility surface. |
| `reload_semantics` | Startup, `settings_changed`, per-frame, on-demand, save-only, test-only, or unknown. |
| `effective_state` | The runtime field, snapshot entry, or explicit `none` state that reflects the setting. |
| `visibility` | Whether the key is editor-visible, hidden from the editor, runtime-only, or dynamically optional. |
| `publicness` | Public, internal, debug-only, deprecated alias, or cleanup candidate. |
| `test_coverage` | Covered, partial, inventory-only, documented gap, needs behavior test, or test-only. |

The deterministic checker is
`modules/gaussian_splatting/tests/check_project_settings_manifest.py`.
It scans production Gaussian C++ source for literal settings and common static
path constants, then fails when a key is referenced without manifest metadata or
when a manifest entry no longer appears in source.

Public setting removals are guarded by
`modules/gaussian_splatting/config/project_settings_public_api_baseline.json`.
Any live public, cleanup-candidate, or deprecated-alias setting must be in that
baseline. A future PR that removes a public setting from source must keep the
baseline entry and add an explicit retired-setting record instead of simply
deleting the manifest entry.

Run it with:

```bash
python3 modules/gaussian_splatting/tests/check_project_settings_manifest.py
```

## Quality Tiers

Quality tiers are policy overrides, not defaults. There is **one** precedence
model (issue #175): **an explicitly-set granular setting always wins over the
tier**; the tier only supplies values for keys the project left at their code
default. Two rules make this concrete:

1. **Pipeline toggles** (`pipeline/enable_packed_stage_data`,
   `enable_tighter_bounds`, `enable_fast_raster`, `enable_sh_amortization`,
   `sh_amortization_divisor`): when
   `rendering/gaussian_splatting/quality/tier_apply_pipeline_toggles` is true and
   a real tier is active, the tier fills only the keys **not** present in
   `project.godot`. An explicit `project.godot` value wins over the tier for its
   key; a `WARN` is logged when the tier value would have differed, and the
   effective snapshot reports `project_override` (not `tier_preset`) for that key.
   An override is detected by the effective value differing from the registered
   default (`property_can_revert`), because `GLOBAL_DEF` marks every key builtin at
   registration; a value explicitly set *equal* to the code default is therefore
   indistinguishable from unset and follows the tier — the same limitation the
   streaming budgets already have.
2. **Streaming budgets**: when
   `rendering/gaussian_splatting/quality/tier_apply_streaming_budgets` is true,
   the budget resolver (`_resolve_tiered_cap_uint`) honors explicit overrides —
   the tier value is used only when no override is present. The separate
   quality/GPU-memory **MIN caps** remain a safety ceiling that still bounds a
   user-set value; they log a `WARN` when they actually reduce a requested value.

Any effective config reported to users must name the true source: `project_override`
for an explicitly-set key, `tier_preset` for a key the tier supplied, `tier_cap`
for a value bounded by a safety cap.

Current provenance surfaces include `PipelineFeatureSet` snapshots and SH tier
seeding. Future cleanup waves should extend equivalent provenance to any
streaming/LOD/quantization setting that can be tier-overwritten.

## Known Cleanup Candidates

The manifest identifies public keys that need separate behavior tests and
migration/deprecation decisions before removal or support changes. The first
wave only inventories them.

Current candidates include:

- `rendering/gaussian_splatting/gpu_sorting_enabled` — deprecated no-op (see S6c
  below); kept registered + bound for ABI, flagged `cleanup_candidate` pending a
  wire-or-remove decision.

(`pipeline/enable_all_experimental` was resolved in S7-renames below: it is
truthfully renamed and no longer a cleanup candidate.)

Removed in settings-hygiene slice S6a: `debug/enable_mainloop_probes`,
`max_gpu_buffer_count`, and `streaming/async_io_enabled` were registered public
keys with zero production readers (no runtime effect). Their `GLOBAL_DEF`
registrations and manifest entries were deleted, and each is now recorded as a
`removed` entry in `config/project_settings_public_api_baseline.json`
(`retired_settings`) while remaining listed in `public_settings` for public API
history. This change is behavior-neutral.

Removed in settings-hygiene slice S6b: `sorting/hybrid_trigger_elements`,
`sorting/hybrid_batch_size`, `pipeline/enable_two_stage_sort`,
`compression/adaptive_chunk_size`, and `streaming/sh_progressive_load` were
"phantom config" keys — each was read into a config field/member, but the
feature it configures is not implemented, so the value never drove a runtime
path. Their registrations, dead config fields, and manifest entries were
deleted; each is now a `removed` entry in `retired_settings` (kept in
`public_settings` for public API history). Re-implementing the underlying
feature is tracked in #480 (hybrid sort), #481 (two-stage sort),
#482 (adaptive chunking), and #483 (progressive SH). This change is
behavior-neutral.

Deprecated in settings-hygiene slice S6c: `gpu_sorting_enabled` is read into a
reported `GaussianSplatManager` flag (bound property + `get_global_stats`) but no
renderer consults it — GPU sorting is always used when available, and CPU sorting
is controlled by `sorting/force_cpu_sort`. Its documentation previously and
falsely claimed "disabling falls back to CPU sorting"; that is corrected. The key,
member, and bound property are kept for ABI (marked `cleanup_candidate` in the
manifest), a one-time WARN fires if a project sets it `false`, and it is NOT
retired/removed (still a live registered setting). This change is behavior-neutral
aside from the new deprecation warning.

Resolved in #167 (settings-hygiene slice 3): `culling/opacity_aware_bounds`,
`culling/visibility_threshold`, and `cull/overflow_autotune_enabled` are no
longer misowned candidates — each is now read by
`GPUCuller::update_culling_settings()` as the project-wide default behind its
live per-renderer `cull/*` property (explicit per-node value still wins), with
behavior-neutral defaults and behavior tests.

Removed in settings-hygiene closeout slice S7: `sorting/onesweep_max_elements`,
`pipeline/sh_amortization_disable_on_visibility_change`,
`pipeline/sh_amortization_visibility_threshold`, `compression/min_chunk_size`, and
`compression/max_chunk_size` were public keys that were read, stored, validated,
and logged but never drove a runtime path. `onesweep_max_elements` was sanitized
and printed by `SortingStrategyConfig`, but the AUTO selector uses only two
boundaries (bitonic->radix and radix->onesweep); the OneSweep band is unbounded
above, so a third boundary is incoherent to wire (no tracking issue). The two
`sh_amortization_*` visibility keys were loaded and validated by `PipelineFeatureSet`
but never reached `render_params` (only `enable_sh_amortization` and
`sh_amortization_divisor` do); the unbuilt SH-recompute-on-visibility-change
feature is tracked in #487. `min_chunk_size`/`max_chunk_size` were loaded, clamped,
and validated by `QuantizationConfig`, but chunk construction uses the fixed
`GaussianStreamingSystem::CHUNK_SIZE` (65536); `min_chunk_size` fed only a cosmetic
compression-ratio estimate whose amortized per-chunk overhead over a 64K-splat
chunk is negligible and was dropped. Adaptive/variable chunk sizing is tracked in
#482. Their registrations, config fields/members, and manifest entries were
deleted; each is now a `removed` entry in `retired_settings` (kept in
`public_settings` for public API history). This change is behavior-neutral.

Renamed in settings-hygiene closeout slice S7 (renames): three misleadingly-named
public keys were renamed to truthful names, each keeping the old spelling as a
read-only **deprecated alias** so existing `project.godot` files keep working. The
effective value is unchanged: the canonical key wins when explicitly set,
otherwise the deprecated alias is read (with a one-time deprecation `WARN`),
otherwise the registered default; a default project (neither set) resolves to the
default with no warning. The deprecated alias is **not** automatically migrated on
save — rename it to the canonical key manually in `project.godot` to stop the
warning.

- `lod/debug_visualization` → `lod/diagnostic_logging` (#167). It is a diagnostic
  LOG-gate (it gates a periodic LOD diagnostic log line and the config-summary
  print), not a visual toggle — it never rendered LOD level colors. The C++ member
  was renamed to `diagnostic_logging`; the `debug_visualization` key in the bound
  LOD stats dictionary is kept for telemetry back-compat.
- `sorting/target_sort_time_ms` → `diagnostics/sort_target_time_ms` (#168). It is a
  telemetry-only logging target (the `meets_target` reference and reported
  `target_ms`), not adaptive. The canonical key now accepts **two** older spellings
  as deprecated aliases; read precedence is `diagnostics/sort_target_time_ms` >
  `sorting/target_sort_time_ms` > `gpu_sorting/target_sort_time_ms`.
- `pipeline/enable_all_experimental` → `pipeline/enable_all_pipeline_experimental`
  (#169). It force-enables exactly the four pipeline experimental toggles
  (`packed_stage`, `tighter_bounds`, `fast_raster`, `sh_amortization`); the old
  name wrongly implied engine-wide scope.

Each old key is flagged `deprecated_alias` in the manifest and kept in
`public_settings` (deprecated aliases are source-parity-exempt and stay live for
API history; they are **not** retired). Each new canonical key was registered with
the same default and editor visibility as the old key and added to the manifest +
`public_settings`. This change is behavior-neutral for the effective value.

## Debug / diagnostic / profiling settings (closeout slice S7-#173)

Slice S7-#173 did three behavior-neutral things: it **removed** one inert key,
**de-duplicated** a double-spelled key, and **labeled** the remaining live debug /
profiling / diagnostic hooks so their scope is truthful and (where they are pure
profiling/debug levers that were editor-visible) so they no longer present as
general runtime controls.

- **Removed (inert):** `debug/enable_state_guardrails`. Its sole consumer,
  `_check_dual_state_sync`, was already deleted, leaving the key an inert
  placeholder that was still exposed as a bound `GaussianSplatRenderer` node
  property and read from `ProjectSettings`. The setting read, the
  `enable_state_guardrails` debug-config member, the `set/get_debug_state_guardrails_enabled`
  methods, the `ADD_PROPERTY` / `bind_method` / doc-class `<member>` + `<method>`
  entries, and the manifest entry were all deleted. It was `debug_only` and not in
  the public API baseline, so no retirement record was needed. No runtime effect.
- **De-duplicated:** `streaming/layout_hint_validation_strict` is now the
  **canonical** layout-hint strict-validation toggle;
  `debug/layout_hint_validation_strict` is a read-only **deprecated alias**. The
  resolver (`gs_layout_hint::_layout_hint_strict_validation_enabled`) reads the
  canonical when explicitly set, else the deprecated alias with a one-time `WARN`,
  else the default (`false`). Neither key is `GLOBAL_DEF`-registered, so
  `has_setting()` is true only when a `project.godot` explicitly carries the key.
  Setting **either** key still enables strict validation, so this is
  behavior-neutral for a project that sets one key. The alias is flagged
  `deprecated_alias` in the manifest and added to `public_settings`.
- **Labeled (editor-visible):** the pure profiling/debug/validation levers below are
  registered with `GLOBAL_DEF` and are clearly labeled in the manifest by `scope`
  (debug / diagnostic) + notes so they no longer read as general runtime controls.
  They remain **editor-visible**: `PROPERTY_USAGE_NO_EDITOR` is **not honored** for
  ProjectSettings — `ProjectSettings::_get_property_list()` overwrites the custom
  usage flags with `EDITOR | STORAGE` for any non-internal setting — so the flag was
  dropped (it was a no-op) and these keys stay visible. Truly hiding them would
  require `ProjectSettings::set_as_internal(path, true)`; the module-wide reliance on
  the ineffective `NO_EDITOR` flag is tracked in
  [#491](https://github.com/klausi3D/godotGS/issues/491).
  `gpu_sorting/subgroup_prefix_mode` is deliberately user-facing (a
  compatibility/workaround lever that routes around driver quirks in the subgroup
  radix-prefix path).

Manifest note: a public-API baseline key must be classified `public`,
`cleanup_candidate`, or `deprecated_alias` (or be retired). These keys therefore
keep `publicness: public` and express their debug/profiling nature via `scope` +
notes, rather than `publicness: debug_only` (which would drop them out of the public
surface and fail the baseline-alignment guard).

| Key | Kind | Scope | Editor |
| --- | --- | --- | --- |
| `debug/enable_pipeline_trace` | Bound node debug toggle + raw read (seeds `debug_config.enable_pipeline_trace`) | debug | Editor-visible (node property; kept) |
| `debug/enable_splat_audit` | Bound node debug toggle + raw read (per-frame GPU buffer audit) | debug | Editor-visible (node property; kept) |
| `debug/splat_audit_sample_count` | Bound node debug parameter + raw read | debug | Editor-visible (node property; kept) |
| `debug/force_unclustered_lights` | Raw-only unregistered debug hook (forces unclustered lighting) | debug | Hidden (unregistered by nature) |
| `gpu_sorting/enable_stage_timestamps` | Per-stage GPU-timestamp profiling | diagnostic | Editor-visible (labeled debug; NO_EDITOR not honored — #491) |
| `gpu_sorting/subgroup_prefix_mode` | Compatibility/workaround lever (auto/force_on/force_off) | compatibility | Editor-visible (kept, by design) |
| `gpu_sorting/enable_prefix_readback` | Debug sync-readback (`DEBUG_ENABLED` only; GPU stall) | debug | Editor-visible (labeled debug; NO_EDITOR not honored — #491) |
| `gpu_sorting/debug_validate_prefix` | Debug prefix cross-check (`DEBUG_ENABLED` only; GPU stall) | debug | Editor-visible (labeled debug; NO_EDITOR not honored — #491) |
| `gpu_sorting/profiling_preserve_gpu_timestamps` | Profiling-only timestamp preservation (`DEBUG_ENABLED` only) | diagnostic | Editor-visible (labeled debug; NO_EDITOR not honored — #491) |
| `sorting/validate_sorted_output` | Debug sorted-key monotonicity validation (GPU stall) | debug | Editor-visible (labeled debug; NO_EDITOR not honored — #491) |

`import/ply_ascii_strict_parse` is listed in issue #173 but does **not** exist on
`master` (0 references in module source); it is already-gone and required no code
change.

## Known Registration Gaps

The manifest also records live runtime reads that need follow-up ownership
decisions:

- `rendering/gaussian_splatting/streaming/max_sync_fallback_loads_per_frame`
- `rendering/gaussian_splatting/streaming/max_sync_fallback_queue_size`
- `rendering/gaussian_splatting/lod/importance_threshold`
- `rendering/gaussian_splatting/cull/frustum_plane_slack`

These remain unchanged in this PR. Follow-up PRs should either register them
with explicit public semantics and behavior tests, or internalize them so they
are not user-facing `ProjectSettings` contracts.
