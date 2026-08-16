# Core fixture set

This directory contains the minimal, versioned core fixture set used by benchmark and QA scenes.

- Set ID: `godot-core-minimal`
- Version: `1.0.0`
- Manifest: `core_fixture_set.json`
- Deterministic assets:
  - `test_splats.ply`
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

`tests/ci/run_baseline_qa.py` calls the script with **no** `--godot-binary`
(`prepare_synthetic_assets()`, invoked from `run_all_tests()`). On a fresh
workspace that lane creates the 1024-splat Python fixture; it no longer
overwrites a floor-valid canonical fixture left by a tests-enabled generator.
The full module-test and runtime-validation consumers pass their selected
binary and require the 10000-splat floor before running.

## `test_splats.gsplatworld` — committed, baked from `test_splats.ply`

The world fixture is the world-route half of the render-route A/B in
`scenes/qa/qa_visual_diff_*.tscn` and `scenes/qa/qa_sh_rotation_*.tscn`. Those
scenes compare a `GaussianSplatWorld3D` render against a `GaussianSplatNode3D`
render of the same content, so the two fixtures must hold the **same splats**.

Before #785 they did not: the committed world was 375 bytes / 10 splats, baked
when `test_splats.ply` was a 918-byte, 10-vertex file (its embedded metadata
still pinned `cache_source_size: 918`). Nothing detected the mismatch, because
the comparison it fed was structurally incapable of failing.

Rebake it from the fixture the QA lane actually uses:

```sh
python tests/runtime/prepare_synthetic_assets.py
bin/godot.windows.editor.dev.x86_64.console.exe --headless \
    --path tests/examples/godot/test_project \
    --script res://scripts/bake_gsplatworld.gd -- \
    --inputs=res://tests/fixtures/test_splats.ply \
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

**You do not have to remember to do this.** The QA route scenes record their
source splat count in the reference manifest and refuse to score when the two
routes disagree (`scripts/qa_route_capture_base.gd`), so a drifted world fixture
fails with a named error instead of an unexplained similarity score.
