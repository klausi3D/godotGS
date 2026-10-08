# Build Your Own Editor

**For:** macOS users, Linux users who want representative speed, and anyone who needs a custom editor build.

**At the end:** you know what to install before you build, and where the build commands are.

On Windows or Linux and just want to try godotGS? A [nightly editor](downloads.md) is quicker.

## What You Need

| Requirement | Details |
| --- | --- |
| Python | 3.10 or newer to run the test and CI scripts under `tests/`; building alone needs 3.8 (enforced by `SConstruct`, untested). CI uses 3.11, except that Linux SCons runs under Ubuntu's system `python3` |
| SCons | 4.0 or newer (enforced by `SConstruct`, untested); CI uses 4.10.1 on Windows and Ubuntu's packaged 4.5.2 on Linux |
| Compiler | Platform C++ toolchain compatible with Godot 4.5 |
| Linux packages | Install the Linux package set listed in [Build from Source](../BUILDING.md) before running `scons` |
| GPU | Vulkan 1.1 minimum for runtime rendering (shaders are compiled to SPIR-V 1.3); tested only on Vulkan 1.4 (NVIDIA GeForce RTX 3090, Windows) |

## Build It

[Build from Source](../BUILDING.md) has the commands for Linux, Windows and macOS. Read its [Build Flavors](../BUILDING.md#build-flavors) section first: the default commands there make a development build, which is slow; use the optimized flavor to evaluate godotGS.

Contributors who will run the guard, QA or runtime validation suites should also read the [Build / Test / CI Command Reference](../reference/build-test-ci.md).

## Next Step

Once you have an editor from this fork, [Your First Splat](quick-start.md) opens the sample project with it, including an optional headless check that the editor loads the project and quits cleanly.
