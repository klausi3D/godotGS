# Installation

Use this page when you need prerequisites, toolchain setup, or an editor built from this fork before [Your First Splat](quick-start.md).

## What You Need

| Requirement | Details |
| --- | --- |
| Python | 3.10 or newer to run the test and CI scripts under `tests/`; building alone needs 3.8 (enforced by `SConstruct`, untested). CI uses 3.11, except that Linux SCons runs under Ubuntu's system `python3` |
| SCons | 4.0 or newer (enforced by `SConstruct`, untested); CI uses 4.10.1 on Windows and Ubuntu's packaged 4.5.2 on Linux |
| Compiler | Platform C++ toolchain compatible with Godot 4.5 |
| Linux packages | Install the Linux package set listed in [Build from Source](../BUILDING.md) before running `scons` |
| GPU | Vulkan 1.1 minimum for runtime rendering (shaders are compiled to SPIR-V 1.3); tested only on Vulkan 1.4 (NVIDIA GeForce RTX 3090, Windows) |

## Choose a Path

| Option | When to use it | Next step |
| --- | --- | --- |
| Reuse a public or local editor | You already have a nightly editor download or a binary you built locally. See [Downloads](downloads.md) for the public nightly binaries. | [Your First Splat](quick-start.md) |
| Build an editor locally | You need a fresh binary from this checkout, or you are on macOS and need a source build. | [Build from Source](../BUILDING.md) |
| Build an editor for validation | You plan to run guard, QA, or runtime validation commands. | [Build / Test / CI Command Reference](../reference/build-test-ci.md) |

## Verify the Editor

Once you have an editor from this fork, [Your First Splat](quick-start.md) opens the sample project with it, including an optional headless check that the editor loads the project and quits cleanly.
