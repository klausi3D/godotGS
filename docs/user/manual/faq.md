# FAQ

Short answers to the questions new godotGS users ask most. Each links to the page with the details.

## Can I use godotGS as a plugin in my normal Godot editor?

No. godotGS is a fork of Godot 4.5 with Gaussian splatting compiled into the engine, so you need its own editor, from [Downloads](../../getting-started/downloads.md) or [built from source](../../BUILDING.md). A stock Godot editor cannot open splat assets.

## Why is the splat visible in one project but not another?

Most often the second project was opened in a stock Godot editor, or it uses the Compatibility renderer. [Nothing Renders](../../troubleshooting/recurring-issues.md#nothing-renders) walks through every cause.

## Which platforms are supported?

Windows and Linux get nightly editors. macOS builds from source but nothing tests it. Android, iOS and the web are not supported: the module is disabled when you build for them. The [Compatibility Matrix](../../reference/compatibility-matrix.md) says what is tested where.

## Which file formats can I import?

`.ply` and `.spz` splat captures, which import as splat assets, and baked `.gsplatworld` world files. See the [Import Workflow](../../workflows/importing.md).

## Can I export and ship a game?

Yes, with a godotGS export template set as `custom_template/release` in your export preset, exported with **Export With Debug** cleared in the export file dialog. Debug exports are not covered yet: no godotGS debug template is published. Without the template the export succeeds but renders no splats. Windows templates come with the nightlies; Linux templates you build yourself. Nothing is code-signed. See [Export Templates](../../development/export-templates.md).

## How fast is it?

On an optimized build with an RTX 3090, a 10,000-splat test scene runs at about 455 frames per second, and the heaviest published test, about 4.9 million visible splats, at about 12. Both use synthetic test data, not real captures. The Linux nightly is not an optimized build, so do not judge speed from it. Numbers and hardware: [Performance Dashboard](../../performance/index.md). To trade quality for speed, start with the [quality presets](performance-presets.md).

## Do splats work with Godot's lights and shadows?

Splats react to scene lights, and a mesh between a directional light and the splats shadows them. Splats do not cast shadows yet. [Lighting Behavior](lighting-behavior.md) covers the controls; [Known Public Alpha Limitations](../../development/known-public-alpha-limitations.md) has the details and a mesh-proxy workaround.

## Is the API stable?

No. Any class, method, property, setting or file format may change in any release while godotGS is alpha. See [API Stability](../../development/api-stability.md).

## Where do I report a problem?

Open an issue on [GitHub](https://github.com/klausi3D/godotGS/issues). [Getting Help](../../troubleshooting/recurring-issues.md#getting-help) lists what to include.

## Where are the build and test commands?

For building an editor: [Build from Source](../../BUILDING.md). For running the test suites as a contributor: [Build / Test / CI Command Reference](../../reference/build-test-ci.md); pass your godotGS editor with `--godot` or `--godot-binary`, because the module tests fail with a stock Godot binary.
