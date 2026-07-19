# Godot Gaussian Splatting (Alpha)

GodotGS is a Godot 4.5 fork with an in-tree Gaussian Splatting module for importing, rendering, and tuning splat-based scenes in the editor and at runtime.
<video src="https://github.com/user-attachments/assets/71542f8d-ccf6-433c-b920-66fdf9ae8c84" autoplay loop muted playsinline></video>

## Download

Nightly editor builds are published as prereleases on GitHub. Pick the latest:

- **[GitHub Releases](https://github.com/klausi3D/godotGS/releases)** - pick the most recent `nightly-YYYYMMDD` entry at the top. The release workflow guarantees the Linux editor tarball when publishing succeeds. Windows editor zips are included when the self-hosted Windows build lane succeeds for that run.
- macOS users currently need to [build from source](docs/BUILDING.md)

No named stable (`v*`) release is published yet, so nightly is the only public install path today. See [Release Channels](docs/development/release-channels.md) for the full publishing model.

## Current Status

| Area | State |
| --- | --- |
| Maturity | Alpha |
| Public binaries | Linux nightly editor; Windows nightly editor when the Windows lane succeeds |
| macOS | Source build first |
| Stable release | Not yet published |
| Compatibility truth | [Compatibility Matrix](docs/reference/compatibility-matrix.md) |
| Performance truth | [Performance Dashboard](docs/performance/index.md) |

## Who This Is For

- Technical artists and graphics engineers evaluating Gaussian Splatting inside a Godot 4.5 fork
- Contributors who need an in-tree module plus engine-patch context, not a standalone plugin
- Reviewers who want to separate the upstream Godot tree from the godotGS-specific delta quickly

## Fastest Way In

1. [Try in 5 Minutes](docs/getting-started/try-in-5-minutes.md) if you want the shortest honest evaluation path.
2. [Public Evaluator](docs/getting-started/quick-start.md) if you want the canonical sample-project flow.
3. [Compatibility Matrix](docs/reference/compatibility-matrix.md) if you need platform evidence before trying it.
4. [Build from Source](docs/BUILDING.md) if you are on macOS or you want a custom editor binary.

## Current Public Evidence

- Compatibility snapshot: Windows is `editor-tested` on the self-hosted Vulkan Forward+ lane with `NVIDIA GeForce RTX 3090` and now ships a nightly editor zip. Linux is `smoke-tested` on `ubuntu-24.04` with `xvfb` and `mesa-vulkan-drivers 25.2.8-0ubuntu0.24.04.1` and ships a nightly editor tarball (the Linux CI lane runs a headless import + runtime smoke check, not a QA-scene lane — see the [Compatibility Matrix](docs/reference/compatibility-matrix.md)). macOS is currently `build-supported`.
- Benchmark snapshot: five committed lanes captured 2026-07-19 at commit `9161d92f349` on an **optimized** build (RTX 3090, Ryzen 7 5800X, Windows 11, Vulkan Forward+). That commit was 8 behind `master` at publication; see [Currency](docs/performance/index.md#currency). Top-line steady-state results — `static_baseline` (10K splats) 455 FPS, `city_flyover` (160K) 129 FPS, `lighting_stress` (90K) 73 FPS, `instance_storm` (360K) 32 FPS, `dense_resident_2m` (4.9M) 12 FPS. The dense lane is published as a **negative** result: the resident path does not reach interactive frame rates at ~5M visible splats. Depth sorting is 64–87% of GPU frame time across all five lanes. Assets are synthetic fixtures, not real captures. Full hardware context, per-pass GPU breakdown, variance, and caveats: [Performance Dashboard](docs/performance/index.md).
- Visual proof: real editor screenshots and short workflow clips are still pending. The current figures are technical diagrams, not product captures. A doctest-driven visual-compare lane now runs in `baseline_qa.yml` against seeded baselines in `tests/visual_baselines/`.

## For Reviewers

- [Reviewer Fast Path](docs/contributor/reviewer-fast-path.md)
- [Engine Patches](ENGINE_PATCHES.md)
- [Architecture Overview](docs/architecture/overview.md)
- [Build / Test / CI Command Reference](docs/reference/build-test-ci.md)

## Documentation

- [Documentation home](docs/index.md)
- [Public roadmap](https://github.com/klausi3D/godotGS/issues/186)
- [Docs site maintenance guide](docs/development/docs-site.md)
- [Contribute](docs/contributor/index.md)
- [User Guide](docs/user/index.md)
- [Reference](docs/reference/index.md)

## Repository Layout

- [Engine root](./): upstream Godot now lives at repository root.
- [Gaussian Splatting module](modules/gaussian_splatting/): module implementation.
- [Test harnesses](tests/): CI and runtime validation tooling.
- [Documentation](docs/): user, contributor, architecture, and reference docs.

## License

Repository code and documentation are MIT-licensed unless noted otherwise.
Upstream engine code at repository root follows upstream licensing.
