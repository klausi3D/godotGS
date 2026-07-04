# ADR: Opt-in importance-based splat pruning in the PLY/SPZ importer

- **Task:** GS-PERF-PRUNE
- **Risk class:** R3 (importer + on-disk/cache format version bump)
- **Status:** Proposed — awaiting CODEOWNER + human approval **before** implementation (R3 gate).
- **Date:** 2026-07-04
- **Baseline:** `eda1c261457`

## Context

Splat count linearly drives our two measured cost centers: GPU sort time (13.5 ms at
dense-2M) and VRAM (144 B/splat resident, or 80 B with the just-landed quantization).
The importer today offers only **uniform** reduction — stride-based density subsampling
(`resource_importer_ply.cpp:_compute_final_splat_count` / the `merge_density` stride) and
a `max_splats` cap. Neither is contribution-aware: they drop splats without regard to how
much each one matters to the rendered image.

Speedy-Splat / LightGaussian-class results show **50–90 % of splats can be dropped by
contribution with little quality loss**. Pruning at *import* shrinks the asset once and
benefits every downstream path — resident, streaming, and quantized alike — unlike a
runtime clamp that re-decides every frame.

The runtime already has a proven, deterministic importance metric and top-k machinery from
PR #420 (`gaussian_importance()`, `select_top_k_indices()`, `compact_chunk_by_importance()`
in `resident_atlas_budget.h`). These are pure functions, reusable at import time.

## Decision

Add **opt-in, off-by-default** importance pruning to the PLY and SPZ importers, applied
**after density-merge and before SH propagation / chunk bake**, reusing the #420 metric and
top-k selection (moved to a shared header without changing the runtime consumer).

### Options (new)

| Option | Default | Meaning |
| --- | --- | --- |
| `processing/prune_ratio` | `1.0` | Keep this fraction of splats by importance (`1.0` = off). |
| `processing/prune_importance_threshold` | `0.0` | Drop splats whose importance is below this absolute value (`0.0` = off). |

Both default to a no-op. When both are set, the ratio and the threshold each produce a keep
set and the **intersection** is kept (a splat must pass both). A new optional preset
**"Optimized"** demonstrates a validated ratio; **preset index 0 remains Ultra**.

### Metric

Reuse `gaussian_importance(g) = clamp(opacity,0,1) * (max(|scale.x|,|scale.y|,|scale.z|) +
1e-4)`, floored to `1e-4` for non-finite inputs (matches #420 exactly). Top-k selection is
`select_top_k_indices()` — `std::nth_element` + sort, **tie-break by ascending source
index** for determinism.

### Known metric bias (explicitly accepted for v1)

`opacity × max-scale` is a **screen-agnostic** proxy. Its known bias: it **over-values large
low-frequency background splats** (big + opaque scores high even if a viewer rarely looks at
it) and **under-values small high-frequency detail** (sharp thin splats that carry edges).
View-contribution metrics (gradient of rendered loss over training views, à la LightGaussian)
would rank better, but **we have no training pipeline / training views**, so a
view-contribution metric is out of scope. The opacity×max-scale metric is the deliberate v1;
its bias is documented so ratios are chosen conservatively and validated on real-scan content
before use. This is why pruning is **opt-in**, never a silent default.

## Consequences

### Determinism & data integrity (hard invariants)

- **Default behavior unchanged:** with options at defaults, output arrays are **byte-identical**
  to base for the same source. Regression-tested; Ultra/Development presets stay lossless
  (`splat_count == source_count`).
- **Deterministic:** same source + same options ⇒ identical output (ascending-index tie-break).
- **Parallel-array consistency:** compaction rewrites *every* parallel array together —
  positions, colors, scales, rotations, opacity_logits, SH bands, palette_ids,
  painterly_flags, normals, brush_axes, stroke_ages. Partial compaction is data corruption;
  a test asserts all arrays end at the same post-prune length.
- **Chunk bake on pruned arrays:** the streaming chunk bake
  (`resource_importer_ply.cpp` bake site) runs on the **post-prune** arrays;
  `streaming_chunk_records` must match the saved arrays exactly (chunk-invariant validators
  must pass).
- **Dual counts in metadata:** record both `original_splat_count` (pre-prune) and
  `splat_count` (post-prune), consistent with the existing density-merge dual-count.

### Format version bump (the R3 crux)

Pruning changes the saved array lengths and the chunk bake, so any cached import of an asset
must be regenerated. Bump the importer format versions — **PLY v7 → v8, SPZ v6 → v7** — so
Godot's import system detects the change and triggers a **clean automatic re-import** of every
existing asset. Rationale:

- **No silent cache ABI break:** without the bump, a stale `.gsplatworld`/cache from v7 could
  be paired with a v8 importer that expects the new metadata, causing subtle corruption. The
  version bump makes the mismatch explicit and self-healing (auto re-import).
- **Backward compatibility:** existing assets re-import automatically at the new version. Because
  pruning is opt-in and Ultra is byte-identical, **no shipped asset silently changes** — an
  asset only shrinks if its `.import` explicitly sets a prune option. Any pruned asset is fully
  restored by re-importing its untouched source PLY/SPZ at ratio 1.0.

### Rollback

Revert the commit(s) **and** the format-version bump together; assets re-import automatically
at the previous version. No shipped asset silently changes (Ultra byte-identical, pruning
opt-in).

## Alternatives considered

- **Runtime-only clamp (#420, already shipped):** re-decides every frame, keeps the full asset
  on disk/VRAM source. Complementary, not a substitute — import pruning shrinks the source once
  for *all* paths.
- **View-contribution metric:** better quality-per-splat, but requires training views we don't
  have. Deferred until/unless a training pipeline exists.
- **Uniform density subsample (existing):** already available; keeps low-importance splats and
  drops high-importance ones indiscriminately. Pruning is the contribution-aware upgrade.

## Validation plan (evidence required before merge)

- Metric determinism + NaN handling at import; ratio and threshold pruning counts;
  parallel-array consistency after compaction; chunk-bake consistency on pruned assets;
  **Ultra-preset byte-identity regression**; existing exact-count importer tests
  (`test_gaussian_importer.h`, `test_ply_importer.h`) still pass unchanged (pruning opt-in).
- **Quality A/B on one real-scan asset** (Grandma's House room) imported at ratio 1.0 / 0.7 /
  0.5, side-by-side screenshots + VRAM and sort-time telemetry for each, with documented
  guidance on which ratios are safe on real-scan content.
- Backward-compat: a pre-existing imported asset re-imports cleanly after the version bump.
- Two independent reviews + CODEOWNER + human approval (R3).

## Open questions for the approver

1. Placement of the shared metric header: move `gaussian_importance()`/`select_top_k_indices()`
   from `renderer/resident_atlas_budget.h` into a neutral shared header (e.g.
   `core/gaussian_importance.h`) that both the importer (`io/`) and the renderer include, so the
   importer does not depend on `renderer/`. Confirm the target location.
2. Preset name/index for "Optimized" and its default ratio (proposal: a validated 0.7).
3. Whether the SPZ importer bump is in scope for the first PR or a fast follow (PLY is the
   primary real-scan path).
