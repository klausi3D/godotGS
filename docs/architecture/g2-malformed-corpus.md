# G2 — malformed-file corpus lane

**Exit criterion G2** (Production-Readiness Program, ledger
[#458](https://github.com/klausi3D/godotGS/issues/458)):

> A malformed-file corpus in CI never aborts / UBs / over-allocates, and all
> savers are atomic.

Phase A hardened four specific hostile-input holes in the import / persistence
read paths (ADR [`adr-import-input-hardening.md`](adr-import-input-hardening.md)).
This lane turns those one-off fixes into a single, blocking, growing gate.

## How it works

- **A single strict gate by tag aggregation.** Malformed-file-**input** rejection
  tests across the import / persistence read paths carry a cross-cutting
  `[MalformedCorpus]` tag in their `TEST_CASE` name. doctest matches the whole
  decorated name, so each case stays in its per-format lane (`[WorldIO]`, `[PLY]`,
  `[SPZ]`, `[Persistence]`, `[Importer]`) **and** joins the aggregate
  `[MalformedCorpus]` lane — no test is moved or duplicated. (Value-level
  rejections that are not file input — e.g. `[Config]` validation of invalid
  config *values* — are intentionally out of scope.)
- **In-test byte generators, no committed blobs.** Most cases write a
  structurally valid file (a **positive control** asserts it loads), then patch
  one field so the matching guard is the sole failure cause; some PLY and
  persistence cases build a synthetic malformed fixture directly, and a couple of
  PLY cases assert correct *handling* (integer-typed-property conversion) of
  hostile-but-legal input rather than rejection. This
  keeps diffs reviewable, deterministic across runners, and clear of the
  tracked-artifact hygiene guard (`tests/ci/run_module_tests.py`).
- **Strict, blocking lanes.** `[MalformedCorpus]`, `[SPZ]`, and `[AtomicWrite]`
  are strict `MODULE_TEST_FILTERS` lanes (`tests/ci/run_module_tests.py`), run by
  the blocking `module-validation` job
  (`.github/workflows/gaussian_production_gates.yml`). A hostile-input or
  crash-atomicity regression hard-fails CI.
- **The final-output writers are locked in statically.** Four `STATIC_FORMAT_GUARDS`
  (`atomic_saver_world_io` / `_scene_serializer` / `_incremental` /
  `_gsplatworld_importer`) assert that *those* final-output writers route through
  `gs_atomic_file_write`. The `[AtomicWrite]` doctest lane proves the helper is
  crash-atomic; the guards prove each writer *uses* it (the PLY cache writer
  delegates to the world saver → covered).

  **Scope limit — this is not "every writer in the module".** The guards cover the
  named final-output writers, not arbitrary `FileAccess::WRITE` sites. A newly
  added final-output writer must both route through `gs_atomic_file_write` and add
  its own guard entry here. This gap was real once:
  `ResourceImporterGSplatWorld::_copy_binary_file`
  (`modules/gaussian_splatting/io/resource_importer_gsplatworld.cpp`) used to open
  its destination with a plain truncating `FileAccess::WRITE`, so a crash or write
  error mid-copy could damage an existing generated output while the other guards
  stayed green (#714). It now routes through the atomic helper and carries the
  `atomic_saver_gsplatworld_importer` guard.

### Regression-guard only (the abort reality)

doctest cannot catch `abort()` / `CRASH_COND`. A genuinely **un-hardened** hole
aborts the process and erases the lane's summary. Therefore **every corpus entry
must already be hardened** — the corpus proves fixes *stay* fixed, it does not
discover new holes. A reverted fix still blocks CI both ways: a fix reverted to a
*wrong error* fails a doctest assertion; a fix reverted to a *re-introduced abort*
kills the (isolated, dedicated) lane process → non-zero exit → strict-lane failure.

### Workflow for a newly discovered hole

1. **Fix first.** Harden the parser so the input fails cleanly (`ERR_FILE_*`,
   never abort/UB/over-alloc). This is an R3 change (persistence read path) under
   the input-hardening ADR — its own PR, CODEOWNER + human approval, GPU/runtime
   evidence as required.
2. **Then add the case.** Add a distilled in-test byte-generator case tagged
   `[…][MalformedCorpus]`, with a positive control, asserting the exact `ERR_FILE_*`.

Never add an un-hardened case — it would abort the lane.

## Coverage

The **authoritative** coverage is the set of `[MalformedCorpus]`-tagged tests
themselves — **60 cases**, distributed `[Persistence]` 20, `[Importer]` 12,
`[PLY]` 11, `[WorldIO]` 10, `[SPZ]` 7. This section summarizes them per read path;
each item corresponds to a tagged case, so it cannot point at a non-existent CI
check.

Do not trust the per-path counts below over the tags — hand-maintained counts
drift (they read 43 total until this was regenerated). Recount from source with:

```
grep -rhoE 'TEST_CASE\("[^"]*\[MalformedCorpus\][^"]*"' modules/gaussian_splatting/tests | wc -l
```

- **WorldIO loader** (`ResourceFormatLoaderGaussianSplatWorld::load`, `[WorldIO]`,
  8): bad magic, wrong version, truncated file, metadata range overflow
  (`fits_within`), high-SH count without the high-SH flag, chunk-index byte-count
  overflow; compressed: `splat_count` over the resident payload cap, and a blob
  that does not decompress to the declared size.
- **gsplatworld importer** (`ResourceImporter*`, `[Importer]`, 3): rejects
  invalid, truncated, and decode-invalid payloads.
- **PLY loader** (`PLYLoader::load_file`, `[PLY]`, 5): missing `end_header`,
  unknown property type, vertex_count out of int range; integer-typed properties
  convert to float (A4), including big-endian byte-swap.
- **PLY importer / ASCII** (`[Importer]`, 2): missing required PLY properties,
  malformed ASCII rows.
- **Asset loader** (`GaussianSplatAsset::load_from_file`, `[Importer]`, 1):
  hard-fails on an unknown raw file extension.
- **SPZ** (`SPZLoader::load_file` — exercised at loader level via `[SPZ]` (7) and
  at importer level via `[Importer]` (6)): bad magic,
  unsupported version, header < 16 B, zero / over-cap point count, `sh_degree` > 3,
  `fractional_bits` > 24 (**A2**); truncated / oversized payload sections,
  oversized decompression claims, header-derived decompression cap, malformed gzip
  optional headers. (SPZ has both loader-level and importer-level coverage.)
- **GSF scene serializer** (`load_scene`, `[Persistence]`, 7): non-chunked
  magic-at-byte0, forward-incompatible version, truncated chunked header, and
  checksum stripped / tampered / zeroed (protected + legacy).
- **Incremental `.gsif` loader** (`[Persistence]`, 4): malformed change tables,
  truncated change-table header, out-of-range and overflow-sized payload slices.
- **Atomic savers** (`gs_atomic_file_write`): the "savers atomic" clause of G2,
  carried by `[AtomicWrite]` (not `[MalformedCorpus]`): a failed write leaves the
  prior file byte-intact, atomic replace with no temp/backup litter, relative-path
  handling — plus four `STATIC_FORMAT_GUARDS` locking in that each final-output
  writer (the three custom savers + the `.gsplatworld` importer copy, #714) routes
  through the helper. The importer copy additionally has a behavioral crash-atomic
  regression under `[Importer]` (an interrupted in-place copy preserves the file).

### Known gaps

- No dedicated **importer-side A1 over-allocation** test — the compressed
  `splat_count` / `gaussian_bytes` overflow is tested against the WorldIO *loader*,
  not `ResourceImporterGSplatWorld`'s validator mirror.
- No test exercises the fallible **`memalloc` OOM-probe** path — the tagged A1 case
  trips the `INT32_MAX` size cap first.
- No **`.gsplatcache` corrupt-cache fallback** test — cache reads reuse the
  world-format guards but are not fed a malformed cache directly.
- No **compressed gzip-bomb within** the `INT32_MAX` cap.
- The **gsplatworld importer's final-output copy** is not crash-atomic — see the
  scope limit under "All savers atomic" above (#714).

## Optional follow-ons (not required for G2)

- **Gap-fill cases:** the four items under [Known gaps](#known-gaps) above. Each
  lands under the fix-first rule if it reveals a new hole.
- **Nightly mutation fuzzer** (discovery, non-blocking): a scheduled
  `continue-on-error` job that mutates valid saver outputs, spawns the binary once
  per file (so an abort kills only that child), and reports candidate holes as
  artifacts. It never auto-feeds the blocking corpus — a human triages → R3 fix →
  distilled case added. This is how the corpus *grows* while staying "hardened-only".

## Running locally

```
python -m SCons platform=windows target=editor dev_build=yes tests=yes -j14
bin/godot.windows.editor.dev.x86_64.console.exe --headless --test --test-case="*][MalformedCorpus]*"
python tests/ci/run_module_tests.py --godot-binary <binary>   # full harness, strict lanes
python tests/ci/run_module_tests.py --guard-only              # static atomic-saver guards
```
