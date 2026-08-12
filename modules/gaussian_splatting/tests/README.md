# Gaussian Splatting Module Tests

_Last updated: 2026-05-18._

This directory contains the test suite for the Gaussian Splatting module.

There are now two parallel test lanes:

1. The classic `--test` lane (CPU-only and tests that lazily probe for a `RenderingDevice`).
2. The `--gs-gpu-test` lane introduced in #333 (doctest tests tagged `[RequiresGPU]`, driven from an offscreen RD-bootstrapped harness — see [GPU Test Harness](#gpu-test-harness) below and the supervisor in `tests/ci/run_gpu_harness.py`).

## Test Structure

The tests are organized using Godot's built-in **doctest** framework:

- `test_gaussian_splatting.h` - Main test header that includes all test suites
- Registration rule: doctest suites that should survive module static-library linking must be included from `test_gaussian_splatting.h`; standalone `.cpp` compilation alone is not a reliable registration path.
- `test_gaussian_data.h` - Tests for core Gaussian data structures
- `test_gpu_streaming.h` - Tests for GPU memory streaming functionality
- `test_gpu_sorting.h` - Tests for GPU-based sorting algorithms
- `test_phase1_integration.h` - Integration tests for the complete rendering pipeline
- `test_painterly_pipeline.h` - Painterly scene regression tests covering shader permutations and headless rendering
- `test_gpu_streaming.cpp` - Extended streaming scenarios, memory pool coverage, and pipeline orchestration
- `test_gpu_sorting.cpp` - Detailed GPU sorter correctness, performance, and stress coverage
- `test_phase1_integration.cpp` - Additional `[GaussianSplatting][Phase1]` coverage for renderer setup and editor workflows
- `test_integration.cpp` - Component wiring smoke test under `[Gaussian Splatting Integration]`
- `test_output_compositor_composite_hazard.h` - 256x256 compositor write-hazard regression for incident #256; ~20 assertions, runs in <1 s under the GPU harness
- `gs_gpu_test_runner.cpp` - Entrypoint for `--gs-gpu-test` (`gs_gpu_test_main`, dispatched from `main/main.cpp` at the `--gs-gpu-test` switch)
- `test_macros.h` - `REQUIRE_GPU_DEVICE()` now owns its fallback RD via `ScopedFallbackRD`, so individual tests no longer need to teardown the device manually

## Building with Tests

To build Godot with tests enabled, run from repository root:

```bash
scons platform=<windows|linuxbsd|macos> target=editor dev_build=yes tests=yes -j4 modules/gaussian_splatting
```

## Running Tests

### Run all Godot tests:
```bash
<godot-editor-binary> --test
```

### Run only Gaussian Splatting tests:
```bash
<godot-editor-binary> --test --test-case="*GaussianSplatting*"
```
This wildcard matches all test cases with the `[GaussianSplatting]` tag.

Typical editor binary names:

- Windows: `bin/godot.windows.editor.dev.x86_64.exe`
- Linux: `bin/godot.linuxbsd.editor.dev.x86_64`
- macOS: `bin/godot.macos.editor.dev.universal`

**Important:** Use `--test-case` (not `--test-suite`) to run all Gaussian Splatting tests, as most tests are defined with `TEST_CASE` macros and exist in the default suite. Using `--test-suite` would skip these tests.

### Painterly regression checks (headless harness):
```bash
godot --headless --script scripts/tools/run_painterly_regression.gd -- --help
```

### Capture/update painterly reference images:
```bash
godot --headless --script scripts/tools/capture_painterly_references.gd -- --help
```

### Painterly stability + demo-readiness checklist:
- See `modules/gaussian_splatting/tests/PAINTERLY_AUDIT_RUNBOOK.md` for reproducible audit commands, expected artifacts, and handoff checklist.

### Deterministic synthetic splat baselines
Use the synthetic generator baseline tool to keep deterministic sample artifacts in sync:

```bash
python3 modules/gaussian_splatting/tests/generate_synthetic_splat_baselines.py
python3 modules/gaussian_splatting/tests/generate_synthetic_splat_baselines.py --check
```

- Baseline artifacts are committed in `modules/gaussian_splatting/tests/synthetic_baselines/`.
- The generator contract is explicit: deterministic `seed` + normalized config produce stable `config_hash` and `scene_hash`.
- Any config or seed change should update at least one of those hashes; use `--check` in QA to detect drift.
- `run_phase1_tests.py --test-only synthetic` runs the same lightweight baseline verification entry point.

### Run with verbose output:
```bash
<godot-editor-binary> --test --test-case="*GaussianSplatting*" --verbose
```
The repository root `run_tests.bat` already passes `--verbose` for you, so you only need to add it manually when invoking the editor yourself.

## GPU Test Harness

The `--gs-gpu-test` entrypoint is a separate doctest runner specifically for tests tagged `[RequiresGPU]`. It is *not* a superset of `--test`; it runs the doctest registry under an offscreen `RenderingDevice` that the harness bootstraps via `Main::test_setup()` + `RenderingContextDriverVulkan` / `RenderingContextDriverD3D12` (no `SceneTree`, no window).

### Direct invocation

```powershell
bin\godot.windows.editor.dev.x86_64.console.exe --gs-gpu-test
bin\godot.windows.editor.dev.x86_64.console.exe --gs-gpu-test --test-case="*HazardRepro*"
bin\godot.windows.editor.dev.x86_64.console.exe --gs-gpu-test --gs-gpu-driver=d3d12
```

Default filter when `--test-case` is omitted: `--test-case=*[RequiresGPU]* --test-case-exclude=*[SceneTree]*,*[Importer]*`. The `[SceneTree][RequiresGPU]` exclusion is tracked as Issue #329 — those tests need a real `SceneTree` and are deferred until the harness grows one.

`--gs-gpu-driver=` accepts `vulkan` (default) or `d3d12`.

### RID leak listener

`gs_gpu_test_runner.cpp` registers a doctest listener that prints

```
[GS-GPU][RID-LEAK?] bytes=<N> test=advisory
```

after each test that leaves more than 4 MiB of GPU memory behind (raised from 1 MiB in #341 to cut benign noise). The supervisor folds any non-zero advisory into a gate failure.

### Supervisor: `tests/ci/run_gpu_harness.py`

The Python supervisor drives `--gs-gpu-test` in **per-batch subprocesses** so a driver hang or GPU OOM in one batch can't corrupt the next. Each batch maps to a `--test-case=` filter:

| Batch | Filter | Status |
| --- | --- | --- |
| `CompositorHazard` | `*HazardRepro*` | Active, **required** (canonical regression) |
| `OutputCompositor` | `*OutputCompositor*][RequiresGPU]*` | Active, **required** (promoted #724) |
| `ComputeInfrastructure` | `*ComputeInfra*][RequiresGPU]*` | Catalogued, empty |
| `TileRenderer` | `*TileRenderer*][RequiresGPU]*` | Active (3 cases, #641) |
| `GpuSorting` | `*Sort*][RequiresGPU]*` | Active, **required** (promoted #744) — gates the #508 `[GPUSortPipeline]` overflow-sticky case AND the three `[GpuSort]` sort-order oracles retagged in #622 (Bitonic + 32-bit/8-bit Radix); `check_gpu_sorting_order_coverage.py` pins that coverage. The `.cpp` order tests remain linker-dropped (#622/#631); the `GPU Sorting Performance` timing benchmark stays advisory |
| `MemoryStream` | `*MemoryStream*][RequiresGPU]*` | Catalogued, empty |
| `Streaming` | `*Streaming*][RequiresGPU]*` | Catalogued, empty |
| `Integration` | `*Integration*][RequiresGPU]*` | Active (1 of 2 cases, #641) |
| `RendererPipeline` | *(named cases)* | Active, **required** |
| `Lifetime` | *(named cases)* | Active, **required** |
| `NodeSceneTree` | `*[Node][SceneTree][RequiresGPU]*` | Active (advisory — flaky under contention, #630) |
| `WorldSceneTree` | `*[World][SceneTree][RequiresGPU]*` | Active, **required** (promoted #724) |
| `SceneDirectorSceneTree` | `*[SceneDirector][SceneTree][RequiresGPU]*` | Active, **required** (promoted #724) |
| `RendererSceneTree` | `*[GaussianSplatting][SceneTree][RequiresGPU]*` | Active, **required** (promoted #724) |

`REQUIRED_BATCHES = {"CompositorHazard", "RendererPipeline", "Lifetime", "OutputCompositor", "RendererSceneTree", "WorldSceneTree", "SceneDirectorSceneTree", "GpuSorting"}` (the four SceneTree/OutputCompositor batches promoted in #724, `GpuSorting` in #744). The set is asserted at import time; an empty filter on a required batch fails the gate, and any hollow (zero-assertion) case in a required batch fails it per-case (#696). `NodeSceneTree` is deliberately advisory — its wall time is only ~1.6× under budget on the shared runner and would be a flaky required gate until #630's contention variance is resolved.

Invocation:

```powershell
python tests\ci\run_gpu_harness.py --batch CompositorHazard --godot bin\godot.windows.editor.dev.x86_64.console.exe
python tests\ci\run_gpu_harness.py --batch CompositorHazard --batch OutputCompositor --godot bin\godot.windows.editor.dev.x86_64.console.exe
python tests\ci\run_gpu_harness.py --godot bin\godot.windows.editor.dev.x86_64.console.exe   # runs every catalogued batch
```

Notes:

- CI strict mode: when `GITHUB_ACTIONS=true` or `CI=true`, `--godot` is required and the supervisor refuses to discover a binary under `bin/`.
- The supervisor writes a JSON report atomically (`tempfile.mkstemp` + `os.replace`).
- Cross-platform streaming uses one thread per pipe — *not* `selectors`, which fails on Windows pipes with a WSAStartup error.

See also the contributor docs in `docs/testing/setup-guide.md` for how this lane sits next to the `--test` lane and the baseline/runtime runners.

### Understanding Test Organization:

**Test Cases vs Test Suites:**
- Most tests use `TEST_CASE("[GaussianSplatting] ...")` and are in the default suite
- Only a few tests use `TEST_SUITE` for explicit grouping:
  - `[GaussianSplatting]` suite – phase-one pipeline tests
  - `[Gaussian Splatting Integration]` suite – component wiring smoke tests

**To run specific tests:**
- For all Gaussian Splatting tests: `--test-case="*GaussianSplatting*"`
- For a specific test suite only: `--test-suite="[GaussianSplatting]"`
- For a specific test case: `--test-case="[GaussianSplatting] GPU Memory Streaming"`

**Note:** The `[Gaussian Splatting Integration]` suite tests don't have the `[GaussianSplatting]` tag, so run them separately with `--test-suite="[Gaussian Splatting Integration]"` or use the `run_tests.bat` script which runs both.

## Quick Test Script

Cross-platform canonical command:

```bash
python3 tests/ci/run_module_tests.py
```

Guard-only validation (fast, no editor required):

```bash
python3 modules/gaussian_splatting/tests/check_build_metadata_consistency.py
python3 modules/gaussian_splatting/tests/check_shader_dependency_contract.py
python3 modules/gaussian_splatting/tests/check_project_settings_manifest.py
python3 modules/gaussian_splatting/tests/check_project_settings_manifest.py --self-test
python3 tests/ci/run_module_tests.py --guard-only
```

The ProjectSettings manifest guard checks
`modules/gaussian_splatting/config/project_settings_manifest.json` against
production Gaussian C++ settings references and keeps
`modules/gaussian_splatting/config/project_settings_public_api_baseline.json`
as the removal/deprecation baseline. Add manifest ownership metadata and a
baseline entry whenever adding a new public `rendering/gaussian_splatting/*`
key; add a retired-settings record instead of deleting a baseline entry.

Windows convenience wrapper (run from a Windows command prompt with SCons and the Visual Studio build tools available in `PATH`):
```bash
run_tests.bat
```
The script builds the Windows editor with tests enabled and then runs `--test-case="*GaussianSplatting*" --verbose` automatically. The CI-oriented `ci\scripts\run_tests.bat` remains for workflows that need to execute the entire engine test suite before the Gaussian Splatting filter.

## Test Categories

### Unit Tests
- **GaussianData basics and layout** – `[GaussianSplatting] GaussianData basic operations` exercises creation, resizing, AABB computation, and empty/single-splat cases, while `[GaussianSplatting] Gaussian structure memory layout` checks GPU-friendly alignment.【F:modules/gaussian_splatting/tests/test_gaussian_data.h†L17-L134】
- **GPU Memory Streaming** – `[GaussianSplatting] GPU Memory Streaming` validates initialization, uploads, triple buffering, overflow handling, and invalid parameters; `[GaussianSplatting] GPU Memory Streaming Performance` enforces timing budgets across dataset sizes.【F:modules/gaussian_splatting/tests/test_gpu_streaming.h†L20-L233】
- **GPU Sorting** – `[GaussianSplatting] GPU Bitonic Sorting` covers initialization, correctness for different sizes, and non-power-of-two inputs, while `[GaussianSplatting] GPU Sorting Performance` benchmarks the sorter and compares against CPU sorting.【F:modules/gaussian_splatting/tests/test_gpu_sorting.h†L21-L204】【F:modules/gaussian_splatting/tests/test_gpu_sorting.h†L205-L347】

### Integration Tests
- **Phase 1 pipeline** – `[GaussianSplatting] Phase 1 Integration - Full Pipeline` drives uploads, async sorting, and frame timing for 100K splats, with additional subcases for concurrent streaming, editor usage, and performance scaling.【F:modules/gaussian_splatting/tests/test_phase1_integration.h†L132-L379】
- **Component wiring** – `[Gaussian Splatting Integration] Component instantiation and linkage` ensures Gaussian data, streaming, and renderer classes can be constructed and connected together.【F:modules/gaussian_splatting/tests/test_integration.cpp†L1-L40】

### Performance Baselines

The tests enforce these performance targets:

| Metric | Target | Test Case |
|--------|--------|-----------|
| 100K splats frame time | < 16.67ms (60 FPS) | Phase 1 Integration |
| 100K splats GPU sort | < 2.0ms | GPU Sorting Performance |
| GPU memory upload | < 1.0ms | Memory Streaming Performance |
| Total GPU memory | < 500MB | Phase 1 Integration |
| Painterly resolve pass | < 2.5ms | Painterly Pipeline Regression |

Painterly datasets live in `tests/painterly_scenes/`. Each JSON descriptor feeds the C++ regression suite, the headless GDScript harness, and the demo scenes (`*_painterly_demo.tscn`) for quick previews in the editor.

## Test Requirements

### Hardware Requirements
- GPU with Vulkan 1.3+ support
- At least 2GB VRAM
- Desktop OS supported by module config (`windows`, `linuxbsd`, `macos`)

### Software Requirements
- Godot built with `tests=yes` flag
- RenderingDevice available (Vulkan-compatible backend)

## Raster Performance Metric Semantics

`RasterPerformance` currently mixes CPU and GPU timing fields. The naming contract is:

- `submission_cpu_ms`: CPU wall-clock time spent recording/submitting tile raster work for the frame.
- `*_gpu_ms` fields (`binning_gpu_ms`, `prefix_gpu_ms`, `raster_gpu_ms`, `resolve_gpu_ms`, `frame_gpu_ms`): GPU execution timings from timestamp queries (possibly delayed by a few frames).
- `tile_assignment_ms` and `rasterization_ms`: high-level renderer timings retained for backward compatibility.

When diagnosing regressions, compare `submission_cpu_ms` against `*_gpu_ms` rather than treating it as a GPU metric.

## Troubleshooting

### Tests skip with "Skipping ... - no RenderingDevice available"
- GPU-heavy suites emit warnings such as `Skipping GPU streaming tests - no RenderingDevice available` or `Skipping GPU sorting performance tests - no RenderingDevice available` when they cannot obtain a `RenderingDevice`. Ensure Vulkan/D3D drivers are installed and Godot is built with GPU backends enabled.【F:modules/gaussian_splatting/tests/test_gpu_streaming.h†L20-L33】【F:modules/gaussian_splatting/tests/test_gpu_sorting.h†L21-L32】【F:modules/gaussian_splatting/tests/test_phase1_integration.h†L132-L145】
- If the device exists but initialization still fails, try launching the editor with `--rendering-driver vulkan` to force the expected backend.

### Build Errors
- Ensure `tests=yes` is included in the scons command
- Clean build: `scons -c` then rebuild
- Check that all test headers are in the tests/ directory

### Performance Test Failures
- Performance targets are conservative for CI environments
- Local development machines should easily exceed targets
- Check for background processes consuming GPU resources

## Adding New Tests

To add new tests to the module:

1. Create a new header file in `tests/` directory
2. Include `tests/test_macros.h` for test framework
3. Use `TEST_CASE()` and `SUBCASE()` macros
4. Include your test header in `test_gaussian_splatting.h`
5. Follow the naming convention: `test_<feature>.h`

Example test structure:
```cpp
#pragma once

#include "tests/test_macros.h"
#include "../core/your_class.h"

namespace TestGaussianSplatting {

TEST_CASE("[GaussianSplatting] Your feature") {
    SUBCASE("Specific test case") {
        // Test implementation
        CHECK(condition);
        REQUIRE(critical_condition);
    }
}

} // namespace TestGaussianSplatting
```

### Waiting for asynchronous pipeline state (#879)

Do **not** warm a renderer up with `for (int i = 0; i < N; i++) render_scene_instance(...)`
and then assert. Streaming residency is produced by the async pack worker threads
(`rendering/gaussian_splatting/streaming/async_pack_enabled`, default on); the
frames only poll it. Measured in #879, first residency needs ~1.2 s of wall clock
and the frame count needed *falls* as the frames are slowed — so a frame budget is
a race against machine speed. A 16-frame warm-up covered 1.2 s on the old runner
and 0.2 s once page heap was removed, and the module's only `has_rendered_content()`
proof went red on unchanged C++.

Use `gs_test_pump.h` instead:

```cpp
#include "gs_test_pump.h"

const GSPumpOutcome warmup = gs_pump_until([&]() {
    renderer->render_scene_instance(&render_data);
    return renderer->has_rendered_content() && renderer->get_visible_splat_count() > 0;
});
if (!warmup.ready) {
    FAIL("Instance pipeline never warmed up ", warmup.describe(),
            " - visible_splat_count=", renderer->get_visible_splat_count());
    return;
}
```

It keeps pumping (it never sleeps — the frames are what drive the async work) and
bounds only the wall clock. The deadline is a **failure**, never a silent skip: if
readiness never arrives the case must `FAIL` and say what never became true.

The deadline also binds the *pass*, not just the loop: `outcome.ready` is true only
for a frame that finished **inside** the deadline, so a slow frame that returns
`true` past the bound is reported as a failure (`describe()` then says the condition
did hold, only too late). Accepting it would put #879 back one level in — a pass a
slow machine can manufacture. The unit proof of that ordering is
`test_gs_pump.h`, in the strict headless `GaussianSplatting [TestPump]` lane.

## CI/CD Integration

The tests are designed to run in CI/CD pipelines:

- All tests complete within 1 minute
- No interactive input required
- Deterministic results (fixed random seeds)
- Clear pass/fail status codes

The `.github/workflows/baseline_qa.yml` workflow runs a `gpu-harness` job after the `gpu-tests` job on the self-hosted Windows runner, both gated against fork-PR code via `head.repo.full_name == github.repository`. The `gpu-harness` job invokes `tests/ci/run_gpu_harness.py` and currently blocks the gate on the `CompositorHazard` batch (incident #256 regression).

## Test Coverage Goals

Current coverage targets:
- Line coverage: >80%
- Branch coverage: >70%
- Critical path coverage: 100%

## Known Limitations

- GPU tests require a physical GPU (no software rendering)
- Some performance tests may vary based on GPU model
- Async compute tests require GPU with async compute support
