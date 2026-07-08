# ADR: Hostile-input hardening of the import / persistence read paths (Phase A)

- **Program:** Production-Readiness (audit 2026-07-07), ledger #458.
- **Risk class:** R3 (persistence / on-disk format read paths).
- **Status:** Proposed — implemented incrementally; each slice is a separate PR
  referencing this ADR. R3 gate: CODEOWNER + human approval before merge.
- **Date:** 2026-07-08
- **Baseline:** `eda1c261457`

## Context

The 2026-07-07 audit found the import/persistence read paths are defense-in-depth
on *structure* (offset/length `fits_within`, checked multiplies, SH-count bounds
— hardened by #438) but still have hostile-input holes that reach **engine abort,
undefined behavior, or silent data corruption** from a crafted or truncated asset
file. A game engine loads asset files as untrusted input (downloaded scenes,
user-supplied captures, shared `.gsplatworld`/`.spz`/`.ply`), so "a malformed
file aborts the engine" is a correctness and trust defect, not a cosmetic one.

Confirmed holes (audit IO subsystem):

- **A1** — compressed `.gsplatworld` never bounds `splat_count`; the loader
  `resize`s the gaussian array before validating it, so a ~100-byte file can
  request hundreds of GB and abort via `LocalVector` `CRASH_COND`. Mirrored in
  the editor importer validator, so *importing* the file crashes the editor.
- **A2** — SPZ `fractional_bits` is read from the header and used as a shift
  (`1 << fractional_bits`) without a range check → shift UB (≥31) and silently
  corrupted geometry.
- **A3** — every persistence writer (`.gsplatworld`, incremental saver, scene
  serializer, PLY cache) truncates the destination in place; a crash mid-save
  destroys the previous good file, including the baseline the incremental-delta
  system depends on.
- **A4** — PLY integer-typed properties (legal PLY) silently parse as `0.0`
  instead of erroring or converting, and the importer validates only splat 0.

## Decision

Harden each read/write path so **malformed input fails cleanly** (`ERR_FILE_*`,
never abort/UB/corruption) and **saves are crash-atomic**, without changing the
on-disk format, the format version, or the behavior for any valid file.

Guiding invariant for the whole phase:

> **Valid files produced by the in-tree savers load and round-trip
> byte-identically after every Phase A change.** The only observable difference
> is that a previously-aborting/UB/corrupting *malformed* input now returns a
> clean error (or, for A3, that an interrupted save leaves the prior file
> intact).

### A1 — compressed `.gsplatworld` splat_count bound (this PR)

The uncompressed path is already bounded structurally by
`fits_within(gaussian_offset, gaussian_bytes, file_len)` — you cannot claim more
splats than the file has bytes. The compressed path decouples `splat_count` from
`file_len`, so it needs an explicit bound (mirrored in the importer validator):

Two guards, both mirrored in the importer validator (all values landed over three
review rounds with Codex on #460):

| Guard | Value / mechanism | Rationale |
| --- | --- | --- |
| Size cap | `gaussian_bytes <= INT32_MAX` (compressed path) | `Compression::decompress` for gzip ERR_FAILs on any destination `> INT32_MAX` (`core/io/compression.cpp` — zlib uses C++ `int`), so a compressed world larger than ~2 GiB decompressed can **never** load. Rejecting it up front is not a false positive; it just moves the guaranteed failure ahead of a pointless multi-GiB `resize`. |
| OOM probe | `memalloc(gaussian_bytes)` before the resize; null → `ERR_OUT_OF_MEMORY` | Even within the size cap, a crafted small file can declare a payload that doesn't fit RAM; `LocalVector::resize` aborts on OOM (`CRASH_COND`). `memalloc` instead returns null (`alloc_static` → `ERR_FAIL_NULL_V(mem, nullptr)`), so probing the allocation fallibly first converts the abort into a clean error on **every** platform. (An earlier revision used `OS::get_memory_info()`, but only Windows implements it — Linux/macOS return -1 — so it was a no-op on the other supported platforms; the probe is platform-independent.) The probe is freed immediately; the resize reuses the just-proven-available memory. Never rejects a payload that actually fits. |

**No ratio cap.** An earlier revision bounded the payload to `256 ×
compressed_size` as an anti-compression-bomb measure. Review correctly flagged that
`save_resident_compressed` gzips raw bytes with no ratio limit, so a highly regular
payload (repeated/zero splats) can legitimately exceed any fixed ratio — the ratio
guard would `ERR_FILE_CORRUPT` a valid, round-trippable saver output, violating the
phase invariant. It was removed in favour of the two guards above.

### A2–A4

Tracked in the ledger; each lands as its own PR under this ADR: A2 a one-line
range check on `fractional_bits`; A3 a shared temp-file→fsync→atomic-rename
helper applied to all four writers; A4 error-or-convert on integer PLY
properties plus wider importer validation. Design detail is recorded per-PR
against this ADR when implemented.

## Consequences

- **No format/version change.** No `.gsplatworld`/`.gsf`/`.spz`/`.ply` file that
  loads today stops loading (except crafted files that *should* be rejected).
  Verified per slice by a load/round-trip test on saver output.
- **New rejections are conservative.** A1's caps only reject payloads that either
  exceed the GPU single-buffer limit (unusable regardless) or are wildly
  disproportionate to their compressed size (not producible by the saver).
- **Constants are duplicated by hand** between the loader and the importer
  validator (following the file's existing convention for magic/version/flags).
  This is flagged for Phase F3 (unify co-bumped constants behind a shared
  header); until then each cap carries a `KEEP IN SYNC` comment.
- **Seeds the G2 malformed-file corpus.** The per-slice regression fixtures
  (crafted/truncated/adversarial-header) become the CI corpus that exit
  criterion G2 requires.
