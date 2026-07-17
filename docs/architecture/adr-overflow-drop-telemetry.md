# ADR: Overflow-drop telemetry (production-readiness C4b / exit criterion G4)

- **Status:** Accepted (owner sign-off 2026-07-16). **Decision D1 = Option B** (resident
  shader flag). See "Decision" below.
- **Risk class:** **R3** (Option B edits `tile_binning.glsl`) → requires this ADR + two
  independent reviews + CODEOWNER approval + a shader-permutation compile check + on-GPU
  evidence. This ADR is the design-note-before-implementation.
- **Program ledger:** #458. Sibling: C4a (#504, 32-bit-sort-engage warn+count) is merged;
  C4b is the second half of G4's "every clamp/overflow drop emits WARN_ONCE + a counter."

## Context / problem

G4 ("no silent degradation") requires every clamp / overflow drop / fallback to emit at
least `WARN_ONCE` plus a diagnostics counter. Two production drop channels exist and
**neither emits a production WARN nor an always-on counter today:**

- **Channel A — tile-binning overlap-record drop.** `shaders/tile_binning.glsl` already
  `atomicAdd`s `overflow_stats.overflow_splats_clamped` when a tile's capacity or the global
  overlap budget is exhausted (records are dropped). The GPU counter **exists**, but the CPU
  readback of the overflow buffer is **gated behind debug flags** (`debug_dump_gpu_counters`,
  or the adaptive/HUD stats path). In a pure production frame (no HUD, no adaptive-tile-size,
  no debug counters) **the drop is counted on the GPU but never read back** → silent.
- **Channel B — instance-count clamp.** `interfaces/gpu_sorting_pipeline.cpp`
  `_on_instance_count_readback` reads `element_count`/`unclamped_total`/`overflow_flag` (from
  `compute/instance_count_clamp.glsl`) **every production frame** (it feeds rendering), but
  surfaces the overflow only through the debug-gated `debug_trace` — no WARN, no counter.

## Decision

- **Channel B (unconditional — R2, CPU only):** in `_on_instance_count_readback` (and the
  sync bootstrap `_capture_instance_count_sync`), when `overflow_flag` is set, emit
  `WARN_PRINT_ONCE` + increment a persistent `instance_count_overflow_events` counter,
  surfaced in the binning debug-stats dict. This is cheap, already production-live, and has
  no design tradeoff.

- **Channel A (the design decision D1):** surface the existing `overflow_splats_clamped`
  drop count in production via **one** of:
  - **Option A (recommended, R2):** an **always-on lightweight async readback** of the tiny
    (`sizeof(OverflowStatsSnapshot)`) overflow buffer, behind a default-on telemetry flag.
    On readback, if `overflow_splats_clamped > 0`, `WARN_PRINT_ONCE` + a persistent
    `overflow_drop_events` counter. Cost: one small async GPU→CPU read/frame + ~2-frame
    latency; **no shader change** → stays R2.
  - **Option B (R3):** a **resident GPU→CPU "overflow occurred" scalar** written by
    `tile_binning.glsl` and read cheaply each frame. Cheaper steady-state signal, but edits
    a hot R3 shader → requires this ADR + two independent reviews + CODEOWNER approval, and
    a shader-permutation compile check.

Recommendation: **Option A + Channel B**, keeping C4b at R2 and honoring the standing
"production render path stays side-effect-free unless a small, explicit telemetry cost is
accepted" invariant. Do not add a new GPU counter — **reuse** the existing
`overflow_splats_clamped` / `overflow_flag`.

## Decision (resolved)

- **D1 — Channel A surfacing = Option B (resident shader flag, R3)**, chosen by the owner
  2026-07-16 for the cheaper steady-state signal. `tile_binning.glsl` writes a resident
  "overflow occurred / drop count" scalar to a small always-resident buffer, read cheaply
  each production frame; on a non-zero value emit `WARN_PRINT_ONCE` + increment a persistent
  `overflow_drop_events` counter (reusing the existing `overflow_splats_clamped` atomic — no
  new GPU counter). This carries the full R3 process: two independent reviews, CODEOWNER
  approval, a shader-permutation compile-matrix entry if the flag is behind a define, and
  on-GPU evidence. Channel B (instance-count) proceeds as R2 in the same effort.

## Evidence plan (both channels)

A real drop can only be provoked on-device (agent PowerShell cannot raster). Use the
self-hosted **GPU harness** lane: render a dense synthetic cloud with per-tile capacity /
`max_overlap_records` forced low (Channel A) and `max_sort_elements` below the visible count
(Channel B); assert the new counters (`overflow_drop_events`, `instance_count_overflow_events`)
go non-zero and the WARN fires once. Classify with `scripts/agentic/classify_change.py`
(fails closed to R3 for any GLSL touch — relevant only if Option B is chosen).

## Consequences

- Production overflow drops become loud (WARN_ONCE) + counted, closing the second half of
  G4. Option A adds a small, bounded per-frame telemetry cost (documented, default-on,
  toggleable); Option B trades that for a hot-shader edit and heavier review. No new GPU
  counters; existing ones are reused.
- **Implementation refinement (review, PR #508):** the resident `overflow_drop_signal` is
  made **sticky** -- the per-frame clear of the overflow-stats buffer excludes the trailing
  signal word, so it persists until the CPU reads it. Otherwise, with the async readback's
  ~2-frame latency and skip-while-pending, a drop that set the signal on frame N+1 could be
  cleared on frame N+2 before frame N's readback (scheduled while the signal was still zero)
  ever observed it -- a silently lost drop, defeating G4. With the sticky flag, whatever drop
  set it survives until a readback reads it; that readback emits `WARN_ONCE` + bumps the
  counter, then re-arms by clearing only the 4-byte signal. Consequently `overflow_drop_events`
  counts **CPU read-intervals in which at least one drop occurred**, not per-frame drops -- it
  is reliably non-zero whenever drops happen (the WARN is the primary signal). Layout parity is
  unchanged: the signal is still a trailing `uint`, still validated by the OverflowStats
  layout-sync guard.
