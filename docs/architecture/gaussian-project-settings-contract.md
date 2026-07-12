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

Quality tiers are policy overrides, not defaults. When
`rendering/gaussian_splatting/quality/tier_apply_pipeline_toggles` or
`rendering/gaussian_splatting/quality/tier_apply_streaming_budgets` is true,
the tier value wins over matching manual project settings. Any effective config
reported to users must name the tier as the source, not claim that the value
came from a default.

Current provenance surfaces include `PipelineFeatureSet` snapshots and SH tier
seeding. Future cleanup waves should extend equivalent provenance to any
streaming/LOD/quantization setting that can be tier-overwritten.

## Known Cleanup Candidates

The manifest identifies public keys that need separate behavior tests and
migration/deprecation decisions before removal or support changes. The first
wave only inventories them.

Current candidates include:

- `rendering/gaussian_splatting/debug/enable_mainloop_probes`
- `rendering/gaussian_splatting/max_gpu_buffer_count`
- `rendering/gaussian_splatting/pipeline/enable_all_experimental`
- `rendering/gaussian_splatting/streaming/async_io_enabled`
- Sorting threshold knobs loaded by `SortingStrategyConfig` but not used by the live AUTO selector policy.
- Quantization chunk-size knobs that currently need producer-path proof before support or removal.

Resolved in #167 (settings-hygiene slice 3): `culling/opacity_aware_bounds`,
`culling/visibility_threshold`, and `cull/overflow_autotune_enabled` are no
longer misowned candidates — each is now read by
`GPUCuller::update_culling_settings()` as the project-wide default behind its
live per-renderer `cull/*` property (explicit per-node value still wins), with
behavior-neutral defaults and behavior tests.

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
