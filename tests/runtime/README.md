# Runtime Validation Harness

`tests/runtime/run_runtime_validation.py` runs runtime harnesses (C++ and GDScript) and writes:

- `tests/runtime/runtime_validation_report.json`

## Scenario Profiles

Runtime scenarios are defined declaratively in:

- `tests/runtime/runtime_scenarios.json`

The canonical headless CI profile is `headless-ci`.
The canonical release-ready profile remains `release-ci` (default for explicit runtime
validation, and the nightly evidence lane — see [CI Integration](#ci-integration)).
The focused canonical node proof profile is `node-asset-gpu-ci`.
The canonical blocking GPU-backed streaming profile is `streaming-gpu-ci`.
`--list-profiles` only lists runtime validation profiles; the benchmark proof surfaces are
separate and live in the benchmark workflow/docs.
List profiles:

```bash
python3 tests/runtime/run_runtime_validation.py --list-profiles
```

Run a specific profile:

```bash
python3 tests/runtime/run_runtime_validation.py --profile stress-only
```

Profile-selected runtime mode is now part of the scenario config. Override it only when
you intentionally need a different execution surface:

```bash
python3 tests/runtime/run_runtime_validation.py \
  --profile streaming-gpu-ci \
  --gd-mode windows-vulkan
```

Override profile selection with explicit tests:

```bash
python3 tests/runtime/run_runtime_validation.py \
  --profile release-ci \
  --gd-test "GPU Streaming Stress" \
  --cpp-test "Runtime Modifications"
```

Use explicit script paths instead of named GDS tests (mutually exclusive with `--gd-test`):

```bash
python3 tests/runtime/run_runtime_validation.py \
  --gd-script tests/runtime/test_gpu_streaming_stress.gd
```

## C++ Harness Link Modes

By default, C++ runtime harnesses compile in `standalone` mode (mock-only).

To exercise module-linked harness builds, pass `--cpp-link-mode module-linked`
and a JSON manifest via `--cpp-build-manifest`:

```bash
python3 tests/runtime/run_runtime_validation.py \
  --cpp-link-mode module-linked \
  --cpp-build-manifest tests/runtime/module_link_manifest.json \
  --skip-gd
```

Manifest schema:

```json
{
  "include_dirs": [".", "modules/gaussian_splatting"],
  "compile_flags": ["-DDEBUG_ENABLED"],
  "link_flags": ["-L/path/to/libs", "-lgaussian_splatting_runtime"]
}
```

## Schema Validation

The runner validates the generated summary payload structure before exit.

- `schema_valid=true` and `schema_errors=[]` are required for a successful run.
- Schema validation failure returns a non-zero exit code.

This makes report-shape regressions fail in CI instead of silently passing.

## CI Integration

Headless CI entrypoints (`make test-runtime`, `tests/ci/run_baseline_qa.py`, and the
headless structural gate in GitHub Actions) execute:

- `--profile headless-ci`

The release-ready runtime profile `release-ci` runs as a **nightly evidence lane**, not
as a required PR gate. `.github/workflows/release_ci_runtime.yml` (triggers: `schedule`
nightly + `workflow_dispatch`) executes it on the self-hosted Windows GPU runner:

- `--profile release-ci --gd-mode windows-vulkan --skip-cpp --fail-on-skip`

`--skip-cpp` mirrors the other self-hosted GPU jobs: the C++ harness compile path uses
`g++`, which is not provisioned on the MSVC runner, so the lane exercises the GDScript
runtime suite plus the required renderer proof. This is an evidence lane; baseline QA CI
does **not** run `release-ci`.

`release-ci` and `node-asset-gpu-ci` require at least one runtime test to emit
`renderer_proof_status=passed` in its `[RUNTIME_METRICS]` payload. A skipped or
unavailable local RenderingDevice is still reported explicitly, but it fails those
proof-required profiles instead of looking like green renderer evidence.

The blocking streaming-specific GPU runtime gate (in `gaussian_production_gates.yml`) uses:

- `--profile streaming-gpu-ci`

This keeps headless CI honest about what can actually execute, wires the broader
release-ready profile as a nightly non-headless evidence lane, and keeps one explicit
blocking streaming gate for world-streaming and residency regressions.

Benchmark evidence collection is separate from the canonical runtime gate and uses the
benchmark runner lane selector instead of runtime validation profiles:

- `openworld-proof-dev` = `open_world_corridor_proof` + `city_flyover`
- `openworld-proof-weekly` = `long_soak`

That benchmark path resolves lane assets through the project-local
`benchmark_asset_manifest.json`. Benchmark classifications in that manifest are
authoritative for what a lane proves. Lanes that still resolve to `test_splats.ply`
are smoke/support evidence only and should not be cited as representative chunked
streaming coverage. That means the current `openworld-proof-dev` surface is a
`20M corridor` candidate plus boundary-crossing smoke support, and
`openworld-proof-weekly` is still city-roam soak smoke support rather than
`100M city` proof. The benchmark surfaces above are evidence-only and do not
create a second blocking streaming gate.

## Synthetic Asset Prep

Runtime and benchmark scenes depend on deterministic synthetic fixtures.

Generate/update them:

```bash
python3 tests/runtime/prepare_synthetic_assets.py --quiet
```

Validate canonical fixture policy:

```bash
python3 tests/runtime/prepare_synthetic_assets.py --check
```

Canonical generated PLY paths:

- `tests/fixtures/test_splats.ply`
- `tests/examples/godot/test_project/tests/fixtures/test_splats.ply`
- `templates/gaussian_splat_template/assets/template_splats.ply`
