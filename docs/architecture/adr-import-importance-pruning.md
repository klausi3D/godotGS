# ADR: Opt-in importance-based splat pruning in the PLY/SPZ importer

- **Task:** GS-PERF-PRUNE
- **Risk class:** R3 (importer + on-disk/cache format version bump)
- **Status:** Design accepted — the four open questions were resolved by the maintainer on
  2026-07-10 (see **Decisions**). Implementation may proceed; the implementation PR still requires
  CODEOWNER + independent review and the validation evidence below (R3 gate).
- **Date:** 2026-07-04 (design), 2026-07-10 (decisions resolved)
- **Baseline:** `eda1c261457`

## Decisions (resolved 2026-07-10)

1. **Shared metric header → `core/gaussian_importance.h`.** `gaussian_importance()` and
   `select_top_k_indices()` depend only on core types (`Gaussian` is in `core/gaussian_data.h`, plus
   `Math` / `LocalVector`), and `io/` has **no** existing dependency on `renderer/`. Moving them to a
   neutral core header is required to keep the importer off `renderer/`; `resident_atlas_budget.h`
   re-includes the core header (runtime consumer unchanged). `compact_chunk_by_importance()` stays in
   the renderer — the importer compacts parallel arrays directly, it does not reuse chunk compaction.
2. **First PR ships options only (no preset).** `prune_ratio` / `prune_importance_threshold` land at
   their no-op defaults (`1.0` / `0.0`); the **"Optimized" preset is a follow-up PR** whose ratio is
   set **from** the A/B results, never a pre-committed guess. This honors the "validate before shipping
   a non-`1.0` default" rule in the Validation plan.
3. **Both PLY and SPZ in the first PR.** The compaction is importer-agnostic (both share an identical
   `_compute_final_splat_count` + parallel-array materialization), so both importers are wired and both
   format versions bump together (PLY 8→9, SPZ 6→7) — **one** re-import event, not two. Because SPZ has
   **no** test coverage today (only PLY has `test_ply_importer.h` + synthetic fixtures), the first PR
   MUST also build synthetic SPZ fixtures + a `test_spz_importer.h` that validate the SPZ v6→v7
   re-import **before** its bump ships.
4. **Empty-result → keep-top-1 (with a `WARN_PRINT`).** If a threshold/ratio would prune to zero, keep
   the single highest-importance splat rather than writing an empty asset — matching the existing
   `final_count = MAX(final_count, 1)` clamp already present in **both** importers. Pruning never
   hard-fails an import; an empty asset is never a valid output.

## Context

Splat count linearly drives our two measured cost centers: GPU sort time (13.5 ms at
dense-2M) and VRAM (144 B/splat resident, or 80 B with the just-landed quantization).
The importer today offers only **uniform** reduction — stride-based density subsampling
(`resource_importer_ply.cpp:_compute_final_splat_count` / the `merge_density` stride) and
a `max_splats` cap. Neither is contribution-aware: they drop splats without regard to how
much each one matters to the rendered image.

The external literature (Speedy-Splat, LightGaussian) **reports** that 50–90 % of splats can be
dropped by contribution with little quality loss. That is motivation, **not** a measured result
for *this* metric on *our* content — those methods use view-contribution metrics we do not have
(see "Known metric bias"). The safe drop fraction for the `opacity × max-scale` proxy below must
be established by local A/B (see Validation plan) before any preset ships a non-`1.0` default.
Pruning at *import* shrinks the asset once and benefits every downstream path — resident,
streaming, and quantized alike — unlike a runtime clamp that re-decides every frame.

The runtime already has a deterministic importance metric and top-k machinery from PR #420
(`gaussian_importance()`, `select_top_k_indices()`, `compact_chunk_by_importance()` in
`resident_atlas_budget.h`), **proven for runtime residency clamping**. They are pure functions,
reusable at import time — but that reuse inherits the same ranking (and the same known bias), so
import quality is *not* implied by their runtime use and must be validated separately.

## Decision

Add **opt-in, off-by-default** importance pruning to the PLY and SPZ importers, reusing the #420
metric and top-k selection (moved to a shared header without changing the runtime consumer).

Insertion point (requires a small refactor): today density selection and SH propagation happen in
the **same materialization loop** (`resource_importer_ply.cpp` bake site), not as separable stages.
Pruning is therefore implemented as a **post-materialization compaction pass** — after the
density-subsampled parallel arrays (positions, SH bands, scales, …) are fully built and **before**
`bake_streaming_chunks_for_asset()` — so it compacts a complete, consistent array set and the chunk
bake runs on the pruned result. It does not interleave into the density loop.

### Options (new)

| Option | Default | Meaning |
| --- | --- | --- |
| `processing/prune_ratio` | `1.0` | Keep this fraction of splats by importance (`1.0` = off). |
| `processing/prune_importance_threshold` | `0.0` | Drop splats whose importance is below this absolute value (`0.0` = off). |

Both default to a no-op. When both are set, the ratio and the threshold each produce a keep
set and the **intersection** is kept (a splat must pass both). Per **Decision 2**, the first PR
ships these two options only (at their no-op defaults); the **"Optimized" preset is a follow-up**
whose ratio is set from the A/B evidence. **Preset index 0 remains Ultra** (lossless default).

### Composition with the existing reducers (ordering, normative)

The importer's two existing count reducers are **not** two independent sequential stages
today: `_compute_final_splat_count(original, max_splats, density)` folds the `max_splats`
cap **into** the density-target count (it returns `min(density_target, max_splats)`), and the
`merge_density` stride then subsamples the source uniformly to reach that final count in a
single materialization loop (`resource_importer_ply.cpp:_compute_final_splat_count` and the
bake loop). So "density then max_splats" is one count computation, not an orderable pipeline.

Importance pruning is inserted as a **distinct, explicit stage** with this normative order:

1. **count computation (existing):** `_compute_final_splat_count` yields the density/`max_splats`
   target `N_density` and the uniform stride that reaches it.
2. **importance prune (this ADR):** on the density-subsampled set, keep the
   `prune_ratio ∩ prune_importance_threshold` subset by importance → `N_pruned ≤ N_density`.
3. **final cap:** `max_splats` is already enforced by step 1 as a hard ceiling on the count. If a
   *separate* post-prune hard cap is ever added, it **must** be importance-aware (re-run top-k) —
   **not** a tail truncation. `select_top_k_indices()` returns kept indices in ascending **source**
   order, not importance order, so slicing its tail would drop arbitrary splats, not the least
   important ones.

Rationale: pruning runs on the density-reduced set so it ranks the splats that actually remain,
and `max_splats` stays a hard ceiling on the final count. The correctness invariant is that the
compacted output is in **source order**; any importance-sensitive truncation must re-select via
top-k, never slice.

### `prune_importance_threshold` is scale-dependent (caveat)

`prune_ratio` is scale-invariant (it keeps a fraction regardless of the asset's units).
`prune_importance_threshold` is an **absolute** importance value, and importance scales with the
asset's world-unit scale (`max(|scale|)` term), so a threshold that is safe on one capture can
wipe out or no-op another. It is therefore an advanced, per-asset knob; `prune_ratio` is the
portable default control. This interacts with the ratio∩threshold intersection above: when both
are set, the kept fraction is not directly predictable from either knob alone.

**Empty-result clamp:** if a threshold (or an extreme ratio) would drop **every** splat, the
importer keeps the single highest-importance splat rather than writing an empty asset — matching
the existing reducers, which clamp the final count to ≥1 (`resource_importer_ply.cpp:_compute_final_splat_count`).
An empty asset is never a valid import output, and pruning never hard-fails an import (Decision 4,
with a `WARN_PRINT` when the clamp engages).

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
must be regenerated. Bump the importer format versions — **PLY v8 → v9, SPZ v6 → v7** — so
Godot's import system detects the change and triggers a **clean automatic re-import** of every
existing asset. (PLY is already at v8 on master as of #467, the A4 integer-decode fix, which took
the v7→v8 slot; this slice therefore takes v9. SPZ is still v6.) Rationale:

- **Two distinct caches, both handled:** the PLY/SPZ importer saves the imported
  `GaussianSplatAsset` as a Godot `.res` (not a `.gsplatworld`); PLY additionally keeps a raw
  decode cache (`.gsplatcache`, `PLY_CACHE_VERSION`, validated by source path/mtime/count). The
  `get_format_version()` bump (PLY 8→9, SPZ 6→7) invalidates the **imported `.res`** and triggers a
  clean auto re-import. The raw `.gsplatcache` is orthogonal — keyed by source identity, not
  importer version — and is unaffected, because pruning happens *after* decode (the raw decode is
  identical). Without the format bump a stale v8 `.res` could pair with a v9 importer's metadata
  expectations, causing subtle corruption; the bump makes the mismatch explicit and self-healing.
- **Backward compatibility:** existing assets re-import automatically at the new version. Because
  pruning is opt-in and Ultra is byte-identical, **no shipped asset silently changes** — an
  asset only shrinks if its `.import` explicitly sets a prune option. Any pruned asset is fully
  restored by re-importing its untouched source PLY/SPZ at ratio 1.0.

### Rollback

Revert the commit(s) **and** the format-version bump together; assets re-import automatically
at the previous version. No shipped asset silently changes (Ultra byte-identical, pruning
opt-in).

Reversibility depends on retaining the source PLY/SPZ: pruning is lossy at the *artifact*
level, so recovery is re-import at ratio 1.0 from the untouched source. If a user deletes the
source after a pruned import, the pruned artifact is the only remaining copy — standard for any
lossy importer setting, but called out because pruning can discard a large fraction of splats.

## Alternatives considered

- **Runtime-only clamp (#420, already shipped):** re-decides every frame, keeps the full asset
  on disk/VRAM source. Complementary, not a substitute — import pruning shrinks the source once
  for *all* paths.
- **View-contribution metric:** better quality-per-splat, but requires training views we don't
  have. Deferred until/unless a training pipeline exists.
- **Uniform density subsample (existing):** already available; keeps low-importance splats and
  drops high-importance ones indiscriminately. Pruning is the contribution-aware upgrade.

## Validation plan (evidence required before merge)

- **Shared-header move** (`core/gaussian_importance.h`, Decision 1): `resident_atlas_budget.h`
  re-includes it and the existing resident-clamp tests still pass unchanged (pure code move).
- Metric determinism + NaN handling at import; ratio and threshold pruning counts;
  **keep-top-1 empty-result clamp** (Decision 4); parallel-array consistency after compaction;
  chunk-bake consistency on pruned assets; **Ultra-preset byte-identity regression**; existing
  exact-count importer tests (`test_gaussian_importer.h`, `test_ply_importer.h`) still pass
  unchanged (pruning opt-in).
- **SPZ test infrastructure (new, Decision 3):** synthetic SPZ fixtures + a `test_spz_importer.h`
  covering exact-count import, Ultra byte-identity, and a clean SPZ v6→v7 re-import — SPZ has no
  test coverage today and its format bump must not ship untested.
- **Quality A/B on one real-scan asset** (Grandma's House room) imported at ratio 1.0 / 0.7 /
  0.5, side-by-side screenshots + VRAM and sort-time telemetry for each, with documented guidance
  on which ratios are safe on real-scan content. **This A/B sets the follow-up "Optimized" preset
  ratio** (Decision 2).
- Backward-compat: a pre-existing imported asset (PLY *and* SPZ) re-imports cleanly after the bump.
- Two independent reviews + CODEOWNER + human approval (R3).

## Open questions — resolved

All four design questions are resolved in **Decisions** (2026-07-10):

1. Shared metric header → `core/gaussian_importance.h` (Decision 1).
2. First PR ships options only; the "Optimized" preset + its validated ratio is a follow-up (Decision 2).
3. Both PLY and SPZ in the first PR, which also builds the missing SPZ test infrastructure (Decision 3).
4. Empty-result → keep-top-1 with a `WARN_PRINT` (Decision 4).

**Still required before the implementation PR merges** (execution gates, not design questions):
CODEOWNER review, one independent review, and the full **Validation plan** evidence above —
including the real-scan A/B that sets the follow-up preset ratio.
