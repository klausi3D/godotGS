# G2 — malformed-file corpus lane

**Exit criterion G2** (Production-Readiness Program, ledger
[#458](https://github.com/klausi3D/godotGS/issues/458)):

> A malformed-file corpus in CI never aborts / UBs / over-allocates, and all
> savers are atomic.

Phase A hardened four specific hostile-input holes in the import / persistence
read paths (ADR [`adr-import-input-hardening.md`](adr-import-input-hardening.md)).
This lane turns those one-off fixes into a single, blocking, growing gate.

## How it works

- **A single strict gate by tag aggregation.** Every malformed-**input**
  rejection test carries a cross-cutting `[MalformedCorpus]` tag in its
  `TEST_CASE` name. doctest matches the whole decorated name, so each case stays
  in its per-format lane (`[WorldIO]`, `[PLY]`, `[SPZ]`, `[Persistence]`) **and**
  joins the aggregate `[MalformedCorpus]` lane — no test is moved or duplicated.
- **In-test byte generators, no committed blobs.** Each case writes a
  structurally valid file (a **positive control** asserts it loads), then patches
  exactly one field so the matching loader guard is the sole failure cause. This
  keeps diffs reviewable, deterministic across runners, and clear of the
  tracked-artifact hygiene guard (`tests/ci/run_module_tests.py`).
- **Strict, blocking lanes.** `[MalformedCorpus]`, `[SPZ]`, and `[AtomicWrite]`
  are strict `MODULE_TEST_FILTERS` lanes (`tests/ci/run_module_tests.py`), run by
  the blocking `module-validation` job
  (`.github/workflows/gaussian_production_gates.yml`). A hostile-input or
  crash-atomicity regression hard-fails CI.
- **"All savers atomic" is locked in statically.** Three `STATIC_FORMAT_GUARDS`
  (`atomic_saver_world_io` / `_scene_serializer` / `_incremental`) assert each
  final-output writer routes through `gs_atomic_file_write`. The `[AtomicWrite]`
  doctest lane proves the helper is crash-atomic; the guards prove each saver
  *uses* it (the PLY cache writer delegates to the world saver → covered).

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

## Coverage matrix

Legend: **✓** covered · **—** n/a / structurally impossible · **○** optional gap
(follow-on).

| Parser entry point | Truncation | Bad magic | Bad version | OOB / overflow counts & offsets | Over-alloc count | Format-specific |
| --- | :---: | :---: | :---: | :---: | :---: | :---: |
| WorldIO uncompressed (`ResourceFormatLoaderGaussianSplatWorld::load`) | ✓ | ✓ | ✓ | ✓ (metadata `fits_within`, SH-flag, chunk-idx) | — (bounded by `fits_within`) | ✓ |
| WorldIO compressed (`kFlagCompressed`) | — | ✓ | ✓ | ✓ (blob mismatch) | ✓ (`splat_count` > INT32_MAX + OOM probe) | ○ gzip-bomb *within* INT32_MAX |
| WorldIO importer validator (editor) | — | — | — | ✓ (mirror cap) | ✓ (mirror) | ○ dedicated mirror test |
| PLY loader (`PLYLoader::load_file`) | ✓ | (implicit) | — | ✓ (vertex_count) | — | ✓ (unknown type, int-typed props, big-endian) |
| PLY cache read (`.gsplatcache`) | via world guards | via world guards | via world guards | via world guards | via world guards | ○ corrupt-cache fallback test |
| SPZ loader (`SPZLoader::load_file`) | ✓ (< 16 B) | ✓ | ✓ | ✓ (num_points 0 / > cap) | ✓ (count cap before sizing) | ✓ (`fractional_bits` > 24 = A2; `sh_degree` > 3) |
| GSF scene serializer (`load_scene`) | ✓ | ✓ | ✓ | ✓ (checksum strip/tamper/zero) | — | ✓ (unknown-chunk skip) |
| Incremental `.gsif` loader | ✓ | — | — | ✓ (bad table, OOR / overflow slices) | ✓ | — |
| Atomic savers (`gs_atomic_file_write`) | ✓ (fail preserves prior) | — | — | — | — | ✓ (no litter, relative-path) + static routing guards |

## Optional follow-ons (not required for G2)

- **Gap-fill cases** (○ above): a compressed gzip-bomb within the INT32_MAX cap,
  a dedicated importer-validator A1 mirror test, and a corrupt-`.gsplatcache`
  fallback test. Each lands under the fix-first rule if it reveals a new hole.
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
