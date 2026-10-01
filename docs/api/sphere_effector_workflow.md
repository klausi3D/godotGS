# Sphere Effector Workflow

## Runtime Surface

This branch supports scene-authored sphere effectors for Gaussian splats through `SphereEffector3D`.

- Author `SphereEffector3D` nodes directly in the scene.
- Match splat nodes with subtree scope, explicit scope roots, world scope, and layer masks.
- Blend each splat node's response with:
  - `rendering/effect_position_scale`
  - `rendering/effect_opacity_scale`
  - `rendering/opacity`
- Drive opacity toward `SphereEffector3D.target_opacity` instead of only dissolving to zero.
- Override wind per node with:
  - `rendering/wind_override_enabled`
  - `rendering/wind_enabled`
  - `rendering/wind_strength`
  - `rendering/wind_direction`
  - `rendering/wind_frequency`

ProjectSettings under `rendering/gaussian_splatting/effects/*` remain available as a backward-compatible fallback when a scene does not author any active `SphereEffector3D` nodes. That fallback stays on the legacy single-global-effector path, so `rendering/gaussian_splatting/effects/max_effectors` is still clamped to `0..1` there even though scene-authored bindings can fill a larger renderer budget.

## What Works In Game

- Wind-only animation on selected splat nodes through node-local wind override.
- Scene-driven sphere position deformation.
- Scene-driven sphere opacity modulation for dissolve or degeneration effects.
- Mixed setups where some nodes react only to wind, some only to sphere position, some only to sphere opacity, and some to both.
- Multiple sphere effectors in one world, with per-instance targeting handled by scene subtree scope, explicit root scope, and layer masks.
- Per-node exclusion from scene effectors by disabling `rendering/scene_effectors_enabled`, zeroing the layer mask, or narrowing `rendering/scene_effector_scope_root`.

## Hard Bounds

- The renderer binds at most `4` scene-authored effectors per pass.
- If more than `4` scene-authored effectors match one node, the highest-priority deterministic four are bound and the rest stay logical matches only.
- Deterministic ordering uses priority first, then scope specificity, then registration order, then object id.
- ProjectSettings fallback effectors remain single-global and are still governed by `rendering/gaussian_splatting/effects/max_effectors` clamped to `0..1`.
- `target_opacity = 1.0` is a neutral target. Matching nodes still count as matched, but no visible opacity change is produced and opacity diagnostics stay inactive.

## Runtime Diagnostics

- `GaussianSplatNode3D.get_last_matched_scene_effector_count()` reports logical matches after scope and layer-mask filtering. This count can stay non-zero even when both active-channel flags are `false`.
- `GaussianSplatNode3D.get_scene_effector_debug_state()` reports both logical and renderer-bound state. `matched_count` is the full logical match set after scope and layer filtering, `bound_count` is the subset that actually fit into the renderer budget, and `truncated` tells you when `matched_count > bound_count`.
- `GaussianSplatNode3D.is_scene_effector_position_active()` only returns `true` when a matched effector can actually contribute position deformation. Zero node position scale or zero effector strength keeps it `false`.
- `GaussianSplatNode3D.is_scene_effector_opacity_active()` only returns `true` when a matched effector can actually contribute opacity modulation. It stays `false` when node opacity is `0.0`, node opacity scale is `0.0`, effector `opacity_strength` is `0.0`, or effector `target_opacity` is `1.0`.
- `GaussianSplatNode3D.get_statistics()` mirrors these runtime values under `matched_scene_effectors`, `bound_scene_effectors`, `scene_effector_truncated`, `scene_effector_position_active`, and `scene_effector_opacity_active`.
- `GaussianSplatNode3D.get_configuration_warnings()` surfaces common inert setups:
  - both effect response scales at `0.0`
  - `rendering/scene_effector_layer_mask = 0`
  - opacity modulation enabled while `rendering/opacity = 0.0`
  - invalid `rendering/scene_effector_scope_root`
- `SphereEffector3D.get_configuration_warnings()` surfaces inert or invalid effector authoring:
  - enabled with neither position nor opacity enabled
  - `opacity_strength = 0.0`
  - `target_opacity = 1.0`
  - `layer_mask = 0`
  - `Parent Subtree` scope without a parent
  - `Explicit Root` scope without `scope_root`

## Authoring Recipes

### Wind-only

1. Disable `rendering/scene_effectors_enabled` on the node, or set both effect scales to `0.0`.
2. Enable `rendering/wind_override_enabled`.
3. Tune `rendering/wind_enabled`, `rendering/wind_strength`, `rendering/wind_direction`, and `rendering/wind_frequency`.

### Sphere Position-only

1. Add a `SphereEffector3D` near the target content.
2. Leave `affect_position = true`.
3. Set the node's `rendering/effect_position_scale` above `0.0`.
4. Set `rendering/effect_opacity_scale = 0.0`.

### Sphere Opacity-only Fade / Dissolve

1. Add a `SphereEffector3D`.
2. Set `affect_opacity = true`.
3. Tune `opacity_strength`.
4. Set `target_opacity`:
  - `0.0` for a dissolve
  - any value below `1.0`, such as `0.35`, for partial degeneration
5. Set the node's `rendering/effect_position_scale = 0.0`.
6. Set `rendering/effect_opacity_scale` above `0.0`.

### Combined Wind + Sphere + Opacity

1. Enable per-node wind override and tune wind values.
2. Enable both position and opacity on the `SphereEffector3D`.
3. Keep both node-local effect scales above `0.0`.

## Artist Notes

- Put a `SphereEffector3D` under the same gameplay parent as the splat nodes you want to affect. The default `Parent Subtree` scope uses the effector's parent as the scope root.
- Switch to `World` scope only when you intentionally want to reach matching splat nodes outside that subtree.
- Use `Explicit Root` together with node `rendering/scene_effector_scope_root` when you need a narrower branch than the effector's default subtree.
- Use effector `layer_mask` and node `rendering/scene_effector_layer_mask` when multiple effect systems share a world.
- Use `priority` when multiple effectors overlap and you need deterministic truncation into the renderer's four-slot scene budget.
- Keep `rendering/effect_opacity_scale` at `0.0` on nodes that should not dissolve even if they still follow position deformation.
- Use `GaussianSplatNode3D.get_scene_effector_debug_state()` or `get_statistics()` from gameplay scripts when you need to understand why an effector matched but did not become renderer-bound, or why a matched effector stayed logically present but inactive.

## Stability And Sanitization

- `GaussianSplatNode3D.rendering/opacity` clamps to `0.0..1.0`.
- `GaussianSplatNode3D.rendering/effect_position_scale` and `rendering/effect_opacity_scale` clamp to `>= 0.0`.
- `SphereEffector3D.radius` clamps to `>= 0.0`.
- `SphereEffector3D.falloff` clamps to `>= 0.001`.
- `SphereEffector3D.frequency` clamps to `>= 0.1`.
- `SphereEffector3D.opacity_strength` clamps to `0.0..1.0`.
- `SphereEffector3D.target_opacity` clamps to `0.0..1.0`.
- Non-finite node or effector values fall back to stable defaults instead of propagating NaNs into rendering.

## Example Scenes

- `tests/examples/godot/test_project/scenes/wind_test.tscn`
- `tests/examples/godot/test_project/scenes/sphere_effector_test.tscn`
