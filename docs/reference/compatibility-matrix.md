# Compatibility Matrix

Evidence last reviewed: 2026-10-01

## Purpose
Use this matrix to track evidence-backed platform status for godotGS Gaussian Splatting.

## Usage
| Task | Action |
| --- | --- |
| Review current platform status | Read the `Current state`, `Public binaries`, and `Notes` columns in the platform table. |
| Update compatibility evidence | Edit `docs/reference/compatibility_sources.yaml` (including `evidence_as_of`) and regenerate this file. |

<figure markdown="1">
![Diagram of the compatibility evidence ladder with current platform positions](../assets/images/compatibility-evidence-ladder.svg){ .gs-diagram }
<figcaption>The matrix is a ladder, not a badge wall: each platform only claims the highest evidence state the repository can currently prove.</figcaption>
</figure>

## Evidence Levels
| Level | Meaning |
| --- | --- |
| `unsupported` | The build system rejects the platform. |
| `build-supported` | The build system accepts the platform and the repository can compile it. |
| `smoke-tested` | A minimal runtime, import, or QA lane passes, but not necessarily a non-headless editor lane. |
| `editor-tested` | A non-headless editor or runtime validation lane passes in repo-owned automation. |
| `sample-project-tested` | A published example-project or QA-scene lane passes on the platform. |
| `production-tested` | Published release-signoff or field evidence exists for the platform. |

## Platform States
The state shown for each platform is the strongest evidence level currently documented in this repository.

| Platform | Current state | Public binaries | Evidence | Notes |
| --- | --- | --- | --- | --- |
| Windows | `editor-tested` | Nightly prereleases attach the Windows editor zip (godotgs-windows-x86_64-<tag>.zip) and the Windows export template zip only when build_windows, build_windows_export_template and export_smoke_windows all succeeded in that release run. When build_windows fails, the nightly ships Linux-only (nightly-20260930, run 36692364878). No stable v* release is published yet. | SUPPORTED_PLATFORMS in modules/gaussian_splatting/config.py accepts windows.<br>Gaussian Production Gates builds a Windows editor and runs the windows-vulkan runtime validation, including the blocking streaming-gpu-ci profile, in its module-validation job (.github/workflows/gaussian_production_gates.yml).<br>Newest green master push run of that job as of the evidence review date: Gaussian Production Gates run 36799027578 (2026-10-01, commit eff00db450c); earlier green ones include runs 36309892055 (2026-09-27) and 34744498012 (2026-09-13). Its runtime-validation-report artifact records streaming-gpu-ci on windows-vulkan as passed=3/3 (GPU Streaming Stress, World Streaming Gate, Streaming Residency API); the tier_1m p95/avg frame-time ratio is an unenforced advisory in that report (#883).<br>Release Builds publish_release (.github/workflows/release_builds.yml) needs build_linux, build_windows, finite_math_guard, release_candidate_gate and export_smoke_windows, and asserts build_linux, finite_math_guard and release_candidate_gate succeeded. On the nightly channel it also requires export_smoke_windows to have succeeded unless build_windows did not succeed; the stable channel always requires it.<br>The shipped bytes are re-verified against the release_candidate_gate attestation, which requires the Windows editor and export template zips whenever build_windows succeeded. fail_on_unmatched_files is true only on the stable channel.<br>Published nightlies: nightly-20260926 (run 36228230720) carries the Windows editor and export template zips; the 2026-09-27 run 36306226275 built Windows but failed export_smoke_windows, so no nightly was published that day; nightly-20260928 to nightly-20260930 (runs 36399911439, 36545382430, 36692364878) are Linux-only because build_windows failed at the self-hosted runner's Configure Git safe.directory setup step.<br>The production evidence route in tests/ci/collect_production_evidence.ps1 exercises non-headless Vulkan runtime scripts on Windows. | Windows is the strongest in-repo validation lane. Windows nightly assets exist only for runs where the Windows build, its export template and the export smoke test all pass; they are not a guarantee for every nightly, and a Windows build that passes but fails the export smoke test blocks that night's publish on every platform. The Release-CI Runtime Evidence lane last passed in run 36314264682 (2026-09-27); its scheduled runs on 2026-09-28, 2026-09-29 and 2026-09-30 (runs 36420796309, 36563672999, 36709070479) failed at the runner's Configure Git safe.directory setup step. That step passes again in Gaussian Production Gates run 36799027578 (2026-10-01), but the lane has not run green since. No stable v* release is published yet. |
| Linux | `smoke-tested` | Guaranteed nightly publish floor: Linux editor tarball in release runs where publishing succeeds. | SUPPORTED_PLATFORMS in modules/gaussian_splatting/config.py accepts linuxbsd.<br>Release Builds compiles and publishes the Linux editor in .github/workflows/release_builds.yml.<br>Baseline QA builds a Linux editor, imports the project, and runs the ply, pipeline, runtime, and module categories headless under xvfb in .github/workflows/baseline_qa.yml (CPU-Only QA Tests job, run_baseline_qa.py --categories ply,pipeline,runtime,module). | Linux remains the fastest public evaluation path and the guaranteed release asset floor. Corrected 2026-07-18 (#596): the Linux CI lane does not select the qa or sorting categories, so no QA-scene or GPU-sorting lane actually runs on Linux — smoke-tested (minimal headless import + runtime lane) is the accurate level, not sample-project-tested (which requires a QA-scene lane to pass). No fresh run ID is added in this docs slice. |
| macOS | `build-supported` | No public macOS binaries at present; build from source. | SUPPORTED_PLATFORMS in modules/gaussian_splatting/config.py accepts macos. | The repo allows macOS builds, but no published smoke, editor, or sample-project evidence is checked in yet. |
| Android | `unsupported` | None. | Android is not listed in SUPPORTED_PLATFORMS in modules/gaussian_splatting/config.py. | The build system rejects this platform. |
| iOS | `unsupported` | None. | iOS is not listed in SUPPORTED_PLATFORMS in modules/gaussian_splatting/config.py. | The build system rejects this platform. |

## Published Test Environments
These rows record the most concrete public environment details currently available from repo-owned automation or published artifacts. Unknown OS, adapter, or driver values are called out explicitly instead of being inferred.

| Platform | State | OS / image | GPU / adapter | Driver / runtime | Evidence | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Windows | `editor-tested` | Self-hosted Windows runner (OS version not yet published) | NVIDIA GeForce RTX 3090 (nvidia-smi output in the run 36799027578 job log) | windows-vulkan runtime mode; Vulkan API and NVIDIA driver versions are not printed in repo-owned evidence | .github/workflows/gaussian_production_gates.yml module-validation lane<br>tests/runtime/run_runtime_validation.py --gd-mode windows-vulkan --profile streaming-gpu-ci<br>tests/ci/collect_production_evidence.ps1<br>Gaussian Production Gates run 36799027578 (master, 2026-10-01) runtime-validation-report: streaming-gpu-ci passed=3/3, failed=0, skipped=0 | Proves the adapter, render route and streaming runtime gate on one self-hosted machine. Windows OS version and NVIDIA driver version are still not printed in repo-owned evidence. |
| Linux | `smoke-tested` | Ubuntu 24.04.4 LTS (image ubuntu-24.04 20260323.65.1) | No discrete GPU exposed in the published artifact (xvfb / headless CPU-only lane) | mesa-vulkan-drivers 25.2.8-0ubuntu0.24.04.1 + xvfb headless import lane | .github/workflows/baseline_qa.yml CPU-Only QA Tests job (ply, pipeline, runtime, module categories only — no qa or sorting category)<br>.github/workflows/release_builds.yml Linux editor build lane | Corrected 2026-07-18 (#596): this row previously claimed sample-project-tested / a passing QA-scene lane, but the Linux CPU-only job does not select the qa category (or sorting), so no QA-scene or GPU-sorting evidence exists for this platform. It is a headless xvfb import + runtime smoke lane, not an interactive editor or hardware-GPU validation row. Not refreshed with a current run ID in this docs slice. |

## Reading the Matrix
- `build-supported` means the build system accepts the platform and repo automation can compile it.
- `smoke-tested` means a minimal runtime or QA lane has passed, but not necessarily an interactive editor lane.
- `editor-tested` means a non-headless editor/runtime lane has passed.
- `sample-project-tested` and `production-tested` are reserved for stronger published evidence than this repo currently exposes publicly.

## Examples
```bash
python3 scripts/update_compatibility_matrix.py
```

## Troubleshooting
| Issue | Cause | Fix |
| --- | --- | --- |
| Matrix does not reflect YAML edits | The generator was not rerun. | Run `python3 scripts/update_compatibility_matrix.py`. |
| A platform only reaches `build-supported` | The repo has no documented runtime or editor evidence for it yet. | Add stronger evidence to `docs/reference/compatibility_sources.yaml` only after the lane or test result exists. |
| OS, adapter, or driver fields are still generic | The lane exists, but those identifiers are not yet published in repo-owned evidence. | Capture them from the evidence run and replace the placeholder text in `docs/reference/compatibility_sources.yaml`. |
| Missing platform row | Platform key was removed from the YAML source. | Re-add the platform entry in `docs/reference/compatibility_sources.yaml`. |
