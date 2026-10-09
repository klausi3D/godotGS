# Gaussian Splat Template Project

_Last updated: 2026-10-09_

This directory contains a self-contained Godot project configured for the GodotGS Gaussian splatting engine. Import `project.godot` in the editor to explore a fully wired scene that demonstrates best practices for using `GaussianSplatNode3D`.

> **The sample asset is generated, not committed.** `.gitignore` excludes
> `templates/**/*.ply`, so a fresh clone has no `assets/template_splats.ply` and
> `scenes/main.tscn` will open with a dangling `ext_resource`. Generate it first —
> see [Getting started](#getting-started) step 1.

## Features

- **Preconfigured project settings** for Forward+ rendering, Vulkan threading, and Gaussian splatting budgets.
- **Autoload bootstrap** (`GaussianBootstrap`) that verifies engine capabilities and exposes module-wide statistics.
- **Template scene** (`scenes/main.tscn`) with:
  - Orbit-ready camera rig. On load the rig frames the splat cloud's bounds;
    it waits for the asset payload to finish uploading rather than reading
    bounds that are still empty at `_ready()`.
  - A `GaussianSplatNode3D` pointing at `assets/template_splats.ply`. No light
    reaches it by default, so a captured scan shows its captured colours — see
    [Showing a captured scan](#showing-a-captured-scan).
  - An opt-in lighting demo: a directional light and a ground mesh, both
    **hidden** by default — see [Lighting demo](#lighting-demo).
  - Painterly rendering is **off** by default, in both the scene and
    `scripts/main_scene.gd`; set `enable_demo_painterly` on the scene root to
    try it.
  - The engine's performance overlay, `GaussianSplatPerformanceOverlay` — see [Performance overlay](#performance-overlay).
- **Input map** tuned for navigation (WASD, Space, C, Shift, RMB orbit, MMB pan, mouse wheel zoom).

## Scene hierarchy

```
GaussianTemplate (Node3D, `scripts/main_scene.gd`)
├── GaussianSplatNode3D (asset + rendering options prefilled)
├── CameraRig (OrbitCameraRig script)
│   └── Camera3D
├── DirectionalLight3D (hidden: lighting demo)
├── Ground (MeshInstance3D, hidden: lighting demo)
├── CanvasLayer
│   └── ControlsHint (Label)
└── PerformanceOverlay (GaussianSplatPerformanceOverlay)
```

## GaussianSplatNode3D inspector defaults

The template applies the recommended inspector values programmatically and in the packed scene:

- **Asset**: assign the imported `GaussianSplatAsset` for `res://assets/template_splats.ply` to `splat_asset`.
- **Quality**: `preset = Balanced`, `lod_bias = 1.0`, `max_render_distance = 150m`, `max_splat_count = 750,000`.
  `max_splat_count` is not a limit on what one splat node draws: the inspector
  shows it only for the `Custom` preset, and an 8,000,000-splat scan drew the
  same 4,117,954 visible splats with `750,000` as with `100,000,000`
  ([#1221](https://github.com/klausi3D/godotGS/issues/1221)).
- **Painterly**: **disabled** by default; the stroke parameters are prefilled (edge threshold `0.25`, stroke opacity `0.85`, stroke width `1.1`, color variation `0.12`, temporal blend `0.35`, seed `1337`) so that enabling it needs one toggle.
- **Rendering**: update when visible, cast shadows (no effect while the demo light is hidden), frustum and occlusion culling on, opacity `1.0`.
- **Debug**: inspector preview and LOD spheres enabled, other overlays off by default, debug draw mode `Off`.

These values mirror the guidance in the Gaussian Splatting inspector documentation and can be tweaked safely to suit your project.

## Getting started

1. Generate the sample asset — it is deterministic (768 splats, sphere, seed
   2202) and is not committed:

   ```bash
   python tests/runtime/prepare_synthetic_assets.py
   ```

   from the repository root. This writes
   `templates/gaussian_splat_template/assets/template_splats.ply`.

   > Note: that command regenerates the **whole** synthetic fixture corpus, not
   > just this asset, and rewrites five tracked manifests. Check `git status`
   > afterwards; they are deterministic regenerations, so you can restore
   > exactly those five and nothing else with:
   >
   > ```bash
   > git restore tests/fixtures/benchmark_asset_manifest.json \
   >   tests/examples/godot/test_project/tests/fixtures/benchmark_asset_manifest.json \
   >   "tests/examples/godot/test_project/tests/fixtures/open_world/*/*.stage_manifest.json"
   > ```
   >
   > (A blanket `git restore tests` would also throw away any unrelated edits
   > you have in that tree.) Giving the generator a per-asset filter is tracked
   > separately.
2. Open the Godot project manager and import `project.godot` from this folder.
3. Press **F5** to run the template scene. The camera frames the splat cloud on
   load and the performance overlay begins updating at 4 Hz.
4. Use the navigation controls listed at the bottom of the window to orbit and inspect
   the splats. **F3** hides the overlay — it is a large panel and it covers the
   middle of the viewport at the default 1280×720.
5. Duplicate `scenes/main.tscn` to bootstrap new levels or swap `default_splat_asset` in `scripts/main_scene.gd` to point at your own imported `.ply` or `.spz` asset.

## Showing a captured scan

A captured 3D Gaussian Splatting scan stores the colour each splat had in the
captured scene, so that scene's lighting is already part of the colour. The
engine adds every visible light in your Godot scene on top of it. A visible
light therefore lights the scan a second time: colours brighten and wash out,
and grass and foliage turn speckled.

Measured on an 8,000,000-splat outdoor scan at 1920×1080 from six camera
poses, against a reference 3DGS renderer (gsplat) at the same poses: with the
demo light visible, the scan rendered 40–58 % brighter than the reference and
PSNR was 15.9–16.8 dB; with the light hidden, it was within −3 % to +9 % of the
reference brightness and PSNR was 24.9–30.2 dB. Frame time changed by less than
5 % ([#1221](https://github.com/klausi3D/godotGS/issues/1221)).

That is why the template ships with no visible light. When you show your own
scan:

- Keep lights out of the scene unless you want to re-light the scan.
- Lights you add for meshes reach the splats too; there is no per-node switch.
  Setting `rendering/gaussian_splatting/lighting/direct_light_scale` and
  `rendering/gaussian_splatting/lighting/shadow_strength` to `0` stops lights
  from changing splat colour for the whole project while meshes stay lit.
- Keep opaque meshes out of the scan's volume: a mesh hides every splat behind
  it. This is why the template's `Ground` is hidden.

## Lighting demo

`DirectionalLight3D` and `Ground` are hidden, not deleted. Make both visible
(the eye icon in the Scene dock) to see the engine light the sample sphere and
the ground under it. `Ground` is a 20 × 20 m box with its top at y = −0.125,
placed for the sample sphere; it cuts off the bottom of the sphere, and it hides
any part of your own scan that lies inside or below it, so move or resize it
before you show it with your own asset.

Keep the `DirectionalLight3D` node even if you never use it: the editor turns
its own preview sun off only while the scene contains a `DirectionalLight3D`,
hidden or not.

## Performance overlay

The overlay is `GaussianSplatPerformanceOverlay`, a class registered by the
engine module (#1084), so any project can add it as a node. It finds the splat
node (or `GaussianSplatWorld3D`) in its viewport's world and the viewport's
camera by itself, and it refreshes every 0.25 s. One rule governs every row:

> **A displayed number is a measurement of the thing its label names, or it is
> not displayed.**

In practice:

- A quantity this build cannot measure renders as `n/a`, never as `0`. A `0` in
  this panel always means "measured, and it was zero".
- **Frame pacing is measured on the wall clock.** FPS is drawn frames divided
  by wall time over the named window, never `1 / delta` of one frame (`delta`
  is scaled by `Engine.time_scale`). The frame interval is labelled as a
  wall-clock interval, because it includes GPU and vsync waits and is not CPU
  time.
- Each GPU pass time is shown only while the renderer's validity flag for that
  pass is set. **That flag is sticky, and the panel says so**: the **Timing
  age** row prints how many frames behind the pass times are.
- The six resolved GPU passes sum to the pass total, and any disagreement is
  printed.
- Rows that come from the process-global custom monitors are shown only when
  those monitors describe the same renderer as the panel; otherwise they read
  `n/a` with the reason.
- The LOD, streaming and SH-compression blocks print
  `no streaming system attached — all rows n/a` when there is none.

**F3** hides the panel (the `hide_key` property changes the key). The raster
policy is shown read-only; set it on the renderer
(`debug/compute_raster_policy`).

For more details on Gaussian splatting workflows, review the documentation in `docs/getting-started/` and `docs/artist_pipeline.md`.
