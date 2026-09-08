# Runtime Validation Harness

`tests/runtime/run_runtime_validation.py` runs runtime harnesses (C++ and GDScript) and writes:

- `tests/runtime/runtime_validation_report.json` (default; override per invocation with
  `--report-path`. The two invocations in `gaussian_production_gates.yml` write distinct
  `runtime_validation_report_<profile>.json` paths so neither profile's report truncates
  the other's, and both are uploaded.)

## Completion Marker (T3 / #891)

`passed` is not the fall-through. A scenario proves completion by printing, on the
terminal success path, exactly one line:

```
[RUNTIME_PASS] {"scenario": "<registry name>", "assertions": <int>}
```

GDScript scenarios emit it through the shared `tests/runtime/gs_runtime_report.gd`
(`ok()` counts each verified check; `emit_pass()` prints the marker); the C++ harnesses
emit it themselves via their `GS_ASSERT` counters. The harness only verifies the marker —
it never synthesises it. The `scenario` field must equal the registry name of the
scenario under classification (mismatch = failure, so a copy-pasted emitter cannot vouch
for another scenario); a malformed payload, a duplicate marker line, or an untracked
`assertions: 0` claim is a failure too. A scenario that legitimately asserts nothing
needs BOTH a `no_assertions_reason` string in the payload AND an unexpired entry in
`runtime_scenarios.json`'s `zero_assertion_allowlist` (fields: `scenario`, `reason`,
`issue_url`, `owner`, `expires_utc`); an entry whose scenario reports `assertions > 0`
fails as stale and must be removed in the same change.

**Advisory ladder (step 1, current):** a clean exit with NO marker classifies as the
advisory status `no_completion_marker` — visible in the report, counted in
`summary["no_completion_marker"]`, printed as `[ADVISORY]`, and not a failure. The
fail-closed flip (step 2) is a later PR gated on a measurable soak: **5 consecutive runs
of each CI-invoked profile (`headless-ci`, `streaming-gpu-ci`, `release-ci`), each
preserved report showing `no_completion_marker == 0` with a marker recorded for every
scenario the profile selected**, plus a code-level assertion that the soaked profiles'
union covers all 11 GDScript registry scenarios. The two C++ scenarios run in no CI lane
(`--skip-cpp` is universal), so their arming evidence is a recorded run without
`--skip-cpp` produced by the flip PR itself. Full contract and the step-2
assertion-floor obligation (ratchet against the immutable review base, per #914 A1):
`run_runtime_validation.py`'s module docstring and
`docs/architecture/adr-phase1-guard-hardening.md` section 4.

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

Generate/update lightweight fixtures (preserving existing floor-valid canonical assets):

```bash
python3 tests/runtime/prepare_synthetic_assets.py --quiet
```

Generate and require the runtime consumer floors:

```bash
python3 tests/runtime/prepare_synthetic_assets.py --quiet \
  --godot-binary ./bin/<your-godot-binary> --require-asset-floors
```

`run_runtime_validation.py` uses this fail-closed form automatically when the
selected registered C++ or GDScript scenario contract declares a floor-governed
fixture; unregistered ad-hoc scripts preflight conservatively because their
indirect dependencies are unknown.
Fixture-free selections (including C++-only `--skip-gd` runs) do not require a
Godot binary for asset preparation. A tests-enabled `run_module_tests.py` lane
uses the fail-closed form before its fixture-consuming module tests; a binary
without test support keeps the runner's strict/warn unavailable-lane policy.

Validate canonical fixture policy:

```bash
python3 tests/runtime/prepare_synthetic_assets.py --check
```

Canonical generated PLY paths:

- `tests/fixtures/test_splats.ply`
- `tests/examples/godot/test_project/tests/fixtures/test_splats.ply`
- `templates/gaussian_splat_template/assets/template_splats.ply`
