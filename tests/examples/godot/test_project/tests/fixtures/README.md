# Core fixture set

This directory contains the minimal, versioned core fixture set used by benchmark and QA scenes.

- Set ID: `godot-core-minimal`
- Version: `1.0.0`
- Manifest: `core_fixture_set.json`
- Deterministic assets:
  - `test_splats.ply`
  - `qa_splats_1024.ply`
  - `test_splats.gsplatworld`

Use these assets as default scene references in repository-tracked test content.
Use CLI/manifest injection only when running alternative stress datasets.

## `test_splats.ply` — generated, never committed

`test_splats.ply` is in `.gitignore`. It is produced by
`tests/runtime/prepare_synthetic_assets.py`, which has **two** generators:

| Invocation | Producer | `test_splats.ply` |
| --- | --- | --- |
| `prepare_synthetic_assets.py` | Preserve an existing floor-valid fixture; otherwise Python fallback (`CANONICAL_SPECS`, seed 1101, sphere, scale 3.0) | Existing **>=10000** splats, or **1024 splats** on a fresh checkout |
| `prepare_synthetic_assets.py --godot-binary <bin>` | C++ `[GeneratePLY]` case in `modules/gaussian_splatting/tests/generate_synthetic_ply_fixtures.h` | 10000 splats |

It is a **benchmark** fixture: the 10000 floor in `ASSET_MIN_SPLAT_COUNTS` needs
the C++ producer. Which producer wrote it depends on the command, so the QA scene
suite does not load it — see `qa_splats_1024.ply` below.

## `qa_splats_1024.ply` — the QA suite's corpus, generated, never committed

The QA scenes under `scenes/qa/` load `qa_splats_1024.ply`, never `test_splats.ply`.
It is also in `.gitignore` and produced by the same script, but only ever by the
Python generator: its name is not in `CPP_GENERATED_FILENAMES`, so passing
`--godot-binary` cannot change it. Its `CANONICAL_SPECS` entry has the same seed,
shape and count as the `test_splats.ply` fallback, so it is byte-for-byte the file
the QA baseline was measured on (1024 splats, 57704 bytes).

The two files used to be one. The QA route pairs compare an instance render of
the PLY against the committed 1024-splat `test_splats.gsplatworld`, and
`tests/ci/baselines/qa_results.json` records `source_splat_count: 1024`. A runner
that generated `test_splats.ply` with the C++ producer then handed the QA suite
10000 splats against a 1024-splat world, and the pair fails as a fixture mismatch
that says nothing about the renderer. Splitting the file gives each consumer the
corpus it needs.

The count is in the name on purpose. Changing it is a baseline change: rebake the
world from the new file (below) and re-measure `qa_results.json` in the same change.

### Which prep commands pass `--godot-binary`

The benchmark evidence surface in `.github/workflows/gaussian_production_gates.yml`,
and the fixture consumers `run_module_tests.py` and `run_runtime_validation.py`,
which also pass `--require-asset-floors` and fail closed. They can: the QA suite
that shares their workspace no longer loads the file they regenerate.
`run_baseline_qa.py` preps the small corpus and says why at the point of prep
(`FALLBACK_CORPUS_REASON`). #790 had pinned all three to the fallback because the QA
corpus *was* `test_splats.ply`; this split is what lifted that pin.

## `test_splats.gsplatworld` — committed, baked from `qa_splats_1024.ply`

The world fixture is the world-route half of the render-route A/B in
`scenes/qa/qa_visual_diff_*.tscn` and `scenes/qa/qa_sh_rotation_*.tscn`. Those
scenes compare a `GaussianSplatWorld3D` render against a `GaussianSplatNode3D`
render of the same content, so the two fixtures must hold the **same splats**.

Before #785 they did not: the committed world was 375 bytes / 10 splats, baked
when `test_splats.ply` was a 918-byte, 10-vertex file (its embedded metadata
still pinned `cache_source_size: 918`). Nothing detected the mismatch, because
the comparison it fed was structurally incapable of failing.

Rebake it from the fixture the QA scenes actually load:

```sh
python tests/runtime/prepare_synthetic_assets.py
bin/godot.windows.editor.dev.x86_64.console.exe --headless \
    --path tests/examples/godot/test_project \
    --script res://scripts/bake_gsplatworld.gd -- \
    --inputs=res://tests/fixtures/qa_splats_1024.ply \
    --output=res://tests/fixtures/test_splats.gsplatworld
cp tests/examples/godot/test_project/tests/fixtures/test_splats.gsplatworld \
   tests/fixtures/test_splats.gsplatworld
```

The bake prints `splats=1024, chunks=1`; both copies must be byte-identical.
The output is uncompressed, which is what makes it a *streamable* world — the
QA world scenes measure `data_source = StreamingGPU` against the instance
scenes' `data_source = ResidentInstanceAtlas`, and a compressed export would
collapse that distinction into a resident-only load
(`docs/workflows/GSPLATWORLD_BAKE.md`).

**You do not have to remember to do this.** Two checks hold the pairing:

* At run time, the QA route scenes record their source splat count in the
  reference manifest and refuse to score when the two routes disagree
  (`scripts/qa_route_capture_base.gd`), so a drifted world fixture fails with a
  named error instead of an unexplained similarity score.
* Statically, in the guard lane, `QaCorpusIsPinnedTest`
  (`tests/ci/test_baseline_qa_require_flag.py`) fails when a QA scene loads a
  floor-governed or C++-produced PLY, or when a route pair's PLY spec count,
  world header count and committed baseline count disagree.
