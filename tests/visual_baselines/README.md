# Visual Baselines

Pixel-stable golden PNGs used by Gaussian Splatting visual-regression tests.

## How baselines are captured

Tests in the module suite that call `TestGaussianSplatting::VisualCompare::capture_and_compare()`
operate in one of two modes:

| Mode | How to trigger | Behavior |
|------|----------------|----------|
| `compare` (default) | no env var, or `GS_VISUAL_BASELINE_MODE=compare` | loads the baseline from this directory and asserts the captured frame matches within tolerance. Missing baseline = test failure with a "run in update mode" hint. |
| `update` | `GS_VISUAL_BASELINE_MODE=update` | writes the captured frame to this directory as the new baseline. Used to (re)capture after a deliberate change. |

The CI workflow `.github/workflows/baseline_qa.yml` already exposes a `baseline_mode`
job input that maps to this env var. Nightly schedule runs auto-update; PR runs compare.

## Canonical capture environment

Baselines must be captured on the project's self-hosted Windows GPU runner —
GitHub Actions label set `[self-hosted, Windows, X64, godotgs, gpu]` — because
PNG output drifts across GPUs and driver versions even when the rendering math
is identical. The same runner is what gates PRs, so a baseline captured anywhere
else will produce false failures.

When the runner's GPU driver is updated, baselines must be re-captured
deliberately. Document each recapture in the commit message:

```
visual_baselines: recapture after Vulkan driver 1.3.275 -> 1.3.296

NVIDIA Game Ready 552.22 -> 555.85 on the godotgs Windows runner.
Compared diffs against previous baselines: all changes are within
sub-LSB rendering tolerances; no functional regression.
```

## Tolerance defaults

`capture_and_compare()` defaults to:

- `max_per_channel_diff_lsb = 1.0` — at most 1 LSB difference per channel per pixel.
- `min_psnr_db = 45.0` — overall PSNR floor.

Both can be tightened per-test where the workload is deterministic enough to
justify it (e.g. solid blits, hazard repro test).

## File layout

```
tests/visual_baselines/
├── README.md                      (this file)
└── <test_name>_<W>x<H>.png        (8-bit sRGB or linear RGBA, format documented per test)
```

PNGs are committed to the repo. Keep file sizes small — prefer 256x256 or
smaller deterministic fixtures over full-viewport captures. The hazard repro
test in `tests/test_output_compositor_composite_hazard.cpp` uses 256x256.

## Current state: the golden-image gate runs, with one baseline

**This section previously said "no PNG baselines exist yet" and that the CI
test runner could not capture one. Both statements are now false** — they
described the state before the GPU harness landed, and were left behind when
it did. `composite_hazard_256x256.png` is committed in this directory and is
compared on every qualifying PR.

The lane is `gpu-harness` in `.github/workflows/baseline_qa.yml`, which runs
`tests/ci/run_gpu_harness.py` on the self-hosted Windows GPU runner (batch
`CompositorHazard`). It resolves `GS_VISUAL_BASELINE_MODE` to `compare` for
PRs and `update` for the nightly schedule, records capture provenance (GPU,
driver, runner, OS build, commit) to `.provenance.json`, and on `update`
opens a recapture PR. Mismatches upload `*.actual.png` as the
`gpu-harness-visual-diffs` artifact.

The workflow for adding a baseline:

1. Add a test that calls `VisualCompare::capture_and_compare(...)`.
2. Run it under `GS_VISUAL_BASELINE_MODE=update` on a GPU host; this writes
   the PNG to this directory.
3. Commit the PNG. If it should block, add it to `blocking_references` in
   `docs/reference/renderer_release_gate_manifest.json` —
   `check_renderer_release_gates.py` then requires the file to exist.
4. PRs run in `compare` mode; mismatch fails the test and writes
   `<baseline>.actual.png` (gitignored, and a tracked one is a hard failure
   via `tracked_actual_png_allowed: false`).

**Coverage is still one image.** Growing it beyond the hazard repro — sort
order, lighting, multi-instance, quantized A/B — is the remaining half of
#522 and is what P0 #184 needs.

## Not to be confused with: the QA scene suite

`tests/examples/godot/test_project/scenes/qa/` and
`tests/ci/baselines/qa_results.json` are a **different** mechanism that also
uses the word "baseline". That one runs GDScript scenes on a real display and
compares a JSON *metric* snapshot (SSIM figures, pixel dominance, and exact
path-identity strings like `route_uid`); it holds no golden images. It was
activated as a blocking lane by #522. Nothing in this directory is used by it.

## Related

- Helper: `modules/gaussian_splatting/tests/visual_compare.h`
- First test: `modules/gaussian_splatting/tests/test_output_compositor_composite_hazard.h`
- CI integration: `.github/workflows/baseline_qa.yml` (baseline_mode input)
