# Gaussian Splatting Effector System — Consumer Guide

This guide is for agents working on game projects that consume the `godotgs` module (e.g. GrandmasHouse). It covers how to correctly author and use the sphere effector + runtime opacity system shipped in #259/#260/#261.

**Scope:** this is not a module internals guide — see `docs/api/sphere_effector_workflow.md` for that. This is the **correct usage** guide for game code and agent-generated scenes.

---

## Three-layer mental model

The effector system has three layers you touch from game code:

1. **`SphereEffector3D`** — authoring node. Defines *what* the effector does.
2. **`GaussianSplatNode3D`** — per-instance filter state. Defines *which* effectors apply to this splat cloud.
3. **Runtime diagnostic API on `GaussianSplatNode3D`** — tells you whether an effector is actually contributing this frame. Use this in gameplay logic instead of poking at world state.

Never walk the scene tree to match effectors to splats yourself. Never call director internals. Use layer 1 + 2 for authoring; use layer 3 for runtime queries.

---

## Authoring a SphereEffector3D (scene file or script)

```gdscript
# Runs inside any Node script (add_child() attaches the effector to this node).
var effector := SphereEffector3D.new()
effector.enabled = true          # default false — must set
effector.radius = 3.0            # world-space units
effector.strength = 1.0          # position amplitude, -10..10
effector.falloff = 2.0           # exponent, >= 0.001
effector.frequency = 2.0         # Hz, >= 0.1
effector.affect_position = true
effector.affect_opacity = false
effector.opacity_strength = 1.0  # 0..1, only used when affect_opacity = true
effector.target_opacity = 0.0    # 0..1, 1.0 = neutral (no change)
effector.layer_mask = 1          # bit field, AND-ed with the node's rendering/scene_effector_layer_mask
effector.scope_mode = SphereEffector3D.SCOPE_SUBTREE  # WORLD, SUBTREE, EXPLICIT_ROOT
effector.priority = 0            # tiebreaker for the 4-slot budget

add_child(effector)
# effector now self-registers via NOTIFICATION_ENTER_TREE
```

### Scope modes — pick one

| Mode | Matches | Use when |
|---|---|---|
| `SCOPE_WORLD` | every `GaussianSplatNode3D` in the same `World3D` | you want a global effect across the whole scene |
| `SCOPE_SUBTREE` (default) | `GaussianSplatNode3D` nodes that are descendants of **the effector's parent** | effector and its splat targets share a parent |
| `SCOPE_EXPLICIT_ROOT` | nodes under the `scope_root` NodePath | surgical targeting, effector and targets aren't siblings |

**SUBTREE is the common default.** If your effector is under `PlayerGroup/EffectorA` and your splat is under `PlayerGroup/SplatA`, default SUBTREE mode matches. If the splat is under `EnemyGroup/SplatB`, it won't — that's correct.

**`SCOPE_EXPLICIT_ROOT` replaces the old `is_ancestor_of` heuristic.** If you need a non-parent ancestor as the scope, use explicit root. The render thread never walks the tree.

### target_opacity semantics

`target_opacity` is what the effector *pushes* each splat's opacity *toward* over the radius/falloff curve. `opacity_strength` is the amount of pushing per frame.

- `target_opacity = 0.0, opacity_strength = 1.0` — full dissolve to transparent inside the sphere.
- `target_opacity = 1.0` — neutral for already-opaque splats. The effector matches but doesn't produce visible change. The runtime diagnostic `is_scene_effector_opacity_active()` returns `false` for this case.
- `target_opacity = 0.3, opacity_strength = 0.5` — push toward 30% opacity at half strength.

**Zero-radius and non-finite transforms are rejected** before the 4-slot budget is applied. Don't rely on them for anything.

---

## Filter state on GaussianSplatNode3D

Every `GaussianSplatNode3D` has three properties under `rendering/` that control which effectors apply.
Grouped property names such as `rendering/scene_effectors_enabled` are single property names that contain a `/`; they cannot be reached with dot access (`splat_node.rendering.scene_effectors_enabled` fails at runtime). Use the bound setters, or `set("rendering/...", value)`:

```gdscript
# assumes: var splat_node: GaussianSplatNode3D
splat_node.set_scene_effectors_enabled(true)       # master toggle (default true)
splat_node.set_scene_effector_layer_mask(0xFF)     # bit-AND with effector.layer_mask (default 1)
splat_node.set_scene_effector_scope_root(NodePath("../SomeAncestor"))  # optional
# Equivalent: splat_node.set("rendering/scene_effector_layer_mask", 0xFF)
```

- `scene_effectors_enabled = false` — the node receives zero scene effectors. Use this for opt-out.
- `scene_effector_layer_mask = 0` — same effect via layer bits.
- `scene_effector_scope_root` — when set, effectors only match if their resolved `scope_root_id` equals this node (explicit scope filter). Leave empty for default subtree matching.

**Don't toggle these every frame.** Each setter bumps the director's instance generation, which invalidates cached uploads. Set at spawn time; change only on state transitions.

### Per-node response scales

Independent of which effectors match, each `GaussianSplatNode3D` has per-channel scale multipliers:

```gdscript
# assumes: var splat_node: GaussianSplatNode3D
splat_node.set_effect_position_scale(1.0)  # default 1.0; inspector range 0..4 (or greater); < 1 dampens position deformation
splat_node.set_effect_opacity_scale(1.0)   # default 1.0; inspector range 0..4 (or greater); < 1 dampens opacity modulation
```

Use these for gameplay fade-in / fade-out without disabling the effector.

---

## Runtime diagnostic API — gameplay should use this

When your gameplay code needs to know "is this splat being deformed right now", **do not** enumerate effectors yourself. Use the runtime API on `GaussianSplatNode3D`:

```gdscript
# assumes: var splat_node: GaussianSplatNode3D
# Is the node's position being pushed this frame?
if splat_node.is_scene_effector_position_active():
    print("play the rustle sound here")  # your game code

# Is opacity being modulated?
if splat_node.is_scene_effector_opacity_active():
    print("start the dissolve VFX here")  # your game code

# Rich diagnostic dictionary for debug overlays / editor tools
var state: Dictionary = splat_node.get_scene_effector_debug_state()
# Keys:
#   matched_count             : int — effectors that pass all filters
#   bound_count               : int — effectors that actually got a GPU slot (<=4)
#   truncated                 : bool — matched_count > bound_count
#   position_active           : bool — any bound effector contributes to position
#   opacity_active            : bool — any bound effector contributes to opacity
#   selected_effector_ids     : Array[int] — ObjectIDs of bound effectors
#   selected_effector_names   : PackedStringArray — node names for display
#   effective_layer_mask      : int
#   scope_filter_present      : bool
#   scope_filter_valid        : bool
#   effective_scope_root_id   : int — resolved scope_root ObjectID
```

### What "active" means

An effector is only counted as *active* for a channel when it can actually contribute on the GPU:
- `position_active` requires non-zero `strength` AND non-zero `effect_position_scale` AND `radius > 0`.
- `opacity_active` requires non-zero `opacity_strength` AND non-zero `effect_opacity_scale` AND `record.opacity > 0` AND `target_opacity != 1.0`.

This matches the shader's actual early-out conditions. If your gameplay asks "is this being affected" and the API says false, the GPU is also producing zero contribution.

---

## Hard budget: 4 slots per renderer pass

The renderer binds at most **4** scene-authored sphere effectors per pass. If more than 4 match one node, the highest-priority deterministic four win:

1. Higher `priority` field first
2. Then higher scope specificity (EXPLICIT_ROOT > SUBTREE > WORLD)
3. Then earlier registration (first-authored wins)
4. Then lower ObjectID (deterministic tiebreak)

`get_scene_effector_debug_state()['truncated']` returns `true` when matched > bound. Use this in editor warnings or telemetry.

**Design implication:** don't author >4 overlapping effectors expecting them to stack. Either spread them across layers, or use priority + scope_root to guarantee the correct 4 win.

---

## Lifecycle — what you don't have to worry about

The module handles these for you. Do not manually register/unregister effectors with the scene director.

- `ENTER_TREE` / `ENTER_WORLD` → auto-register
- `EXIT_TREE` / `EXIT_WORLD` → auto-unregister
- `TRANSFORM_CHANGED` → auto-resync transform
- Any property setter → auto-push new state
- World3D switch mid-tree → migrated automatically

What you *do* have to worry about:

- **Don't add `SphereEffector3D` outside a tree.** It only registers on `ENTER_TREE`. Instantiating and mutating without adding as a child does nothing.
- **Don't set `scope_root` to a NodePath that resolves to a non-ancestor.** The resolution check rejects it and the effector is scoped to `ObjectID()` (no match). Editor config warning surfaces this; respect it.

---

## Common pitfalls

### "My effector isn't affecting anything"

Check in order:

1. `effector.enabled == true` (defaults to `false`).
2. `effector.radius > 0`.
3. `effector.affect_position || effector.affect_opacity`.
4. At least one channel can contribute: `strength != 0` for position, `opacity_strength != 0 && target_opacity != 1.0` for opacity.
5. `effector.layer_mask & splat_node.get_scene_effector_layer_mask() != 0` (the node's `rendering/scene_effector_layer_mask` defaults to `1`).
6. Scope matches: for `SCOPE_SUBTREE`, the effector's parent must be an ancestor of the splat node. For `SCOPE_EXPLICIT_ROOT`, the NodePath must resolve to a live Node that's an ancestor.
7. `splat_node.is_scene_effectors_enabled() == true` (`rendering/scene_effectors_enabled`).

Query `splat_node.get_scene_effector_debug_state()` and inspect `matched_count` + `bound_count`. If matched is 0, filter mismatch. If matched > 0 but bound is 0, slot budget exhausted by higher-priority effectors.

### "I added 6 effectors and only 4 apply"

Expected. See the budget section. Either reduce count or use `priority` / `scope_root` to control which 4 survive.

### "Opacity doesn't change"

Check `target_opacity`. `1.0` means "push to fully opaque" — for already-opaque splats, that's a no-op. You probably wanted `0.0` (dissolve) or a small value.

### "My animation of `target_opacity` (or `strength`) isn't reflected in the cached raster"

Fixed — all numeric fields (including `target_opacity`) participate in the scene-effector cache signature. If you see this, file a bug; it regressed.

### "Effector stopped matching after I moved its parent"

Expected if scope is `SCOPE_SUBTREE`. The implicit scope is the effector's parent — if the parent no longer is an ancestor of the splat node, no match. Use `SCOPE_EXPLICIT_ROOT` with a stable `scope_root` NodePath for mobile hierarchies.

---

## Don't do

- **Don't** enumerate effectors by scanning for `SphereEffector3D` and applying effects yourself. The shader does it.
- **Don't** call `GaussianSplatSceneDirector::register_sphere_effector` directly. The node does it.
- **Don't** write to `effect_params[2]` in any GPU buffer — it's the scene-effector mask, computed by the director.
- **Don't** expect deterministic slot indices across worlds — the 4 slots are per-world-pass. An effector in World A has no relationship to slot 0 in World B.
- **Don't** rely on `target_opacity` reaching exactly 0 or 1 via the effector. It's a blend target; the amount applied depends on radius + falloff + opacity_strength.

---

## Minimum working example

```gdscript
# Scene:
# Root
# └── Group (Node3D)
#     ├── Splat (GaussianSplatNode3D, asset loaded)
#     └── Effector (SphereEffector3D)

# Group.gd
extends Node3D

func _ready() -> void:
    var effector: SphereEffector3D = $Effector
    effector.enabled = true
    effector.radius = 2.5
    effector.strength = 1.0
    effector.affect_position = true
    # No other config needed. SCOPE_SUBTREE + layer_mask=1 + priority=0 are fine defaults.
    # On ENTER_TREE the effector registers itself; Splat will be affected because
    # Group is the effector's parent, which is also an ancestor of Splat.

func _process(_delta: float) -> void:
    var splat: GaussianSplatNode3D = $Splat
    if splat.is_scene_effector_position_active():
        $AudioStreamPlayer3D.playing = true
```

That's it. No director calls, no scene-tree walks, no manual signal wiring.

---

# Part 2 — Tutorial & Recipes (for game design agents)

Part 1 is reference material. Part 2 is practical: *how do I make X happen in the demo prototype.*

**Reference scene:** `tests/examples/godot/test_project/scenes/sphere_effector_test.tscn` in the `godotgs-clean` repo is the canonical visual regression scene. It shows all the channels side-by-side (wind-only, position-only, opacity-only, combined, and a moving-effector row). Open it to see the effects working before copying the setup into GrandmasHouse.

## Your first effector, step-by-step

Goal: make a Gaussian-splat prop dissolve when the player walks near it.

1. Put the splat node in the scene:
   ```
   PropRoot (Node3D)
   └── Prop (GaussianSplatNode3D, asset set)
   ```
2. Add a `SphereEffector3D` as a sibling *under the same parent* (SCOPE_SUBTREE finds it that way):
   ```
   PropRoot
   ├── Prop
   └── DissolveField (SphereEffector3D)
   ```
3. Configure the effector:
   - `enabled = true`
   - `radius = 2.0` (world-space meters)
   - `affect_position = false`
   - `affect_opacity = true`
   - `opacity_strength = 1.0`
   - `target_opacity = 0.0`    — push to invisible
   - `falloff = 0.5`           — gentle edge, more of the prop dissolves than just the center
4. Parent the `DissolveField` under whatever you want to track the player (or move it per-frame yourself). The effector follows its node transform automatically.
5. Run. When the field overlaps the prop, it dissolves inside the sphere.

No director calls. No per-frame re-registration. The node handles it.

## Recipes

### Recipe A — Fade a prop out on proximity (dissolve VFX)

```gdscript
# PropWithDissolve.gd (attached to PropRoot)
extends Node3D

@onready var effector: SphereEffector3D = $DissolveField
@onready var player: Node3D = get_tree().get_first_node_in_group("player")

func _process(_delta: float) -> void:
    if player:
        effector.global_position = player.global_position  # field follows player
```

Authoring in the scene (Inspector):
- `DissolveField.radius = 2.5`, `affect_opacity = true`, `target_opacity = 0.0`,
  `opacity_strength = 1.0`, `falloff = 0.5`, `affect_position = false`.

### Recipe B — Magic wand / spell field (moving effector)

Same shape as the `MovingEffector` in the reference scene. Attach a ping-pong or follow-mouse script to the effector transform.

```gdscript
extends SphereEffector3D

var _t := 0.0
func _process(delta: float) -> void:
    _t += delta
    position = Vector3(sin(_t * 0.5) * 10.0, 2.0, 0.0)
```

Tune for "strong sweep" (matches the reference scene's moving row):
- `radius = 10.0`, `falloff = 0.3` — wide, sharp-edged
- `affect_position = true`, `strength = 1.5` — splats displace outward in the wake
- `affect_opacity = true`, `target_opacity = 0.0`, `opacity_strength = 1.0` — full dissolve inside
- On each receiving `GaussianSplatNode3D`: `rendering/effect_position_scale = 1.0`, `rendering/effect_opacity_scale = 1.0`.

### Recipe C — Foliage / curtain wind sway

Sphere effectors do radial motion, which is *not* what you want for wind. Use the instance-wind override instead:

```gdscript
# assumes: var foliage_splat: GaussianSplatNode3D
foliage_splat.set_wind_override_enabled(true)   # rendering/wind_override_enabled
foliage_splat.set_wind_enabled(true)            # rendering/wind_enabled
foliage_splat.set_wind_strength(1.0)            # rendering/wind_strength
foliage_splat.set_wind_direction(Vector3(1, 0, 0.2).normalized())  # rendering/wind_direction
foliage_splat.set_wind_frequency(1.5)           # rendering/wind_frequency
```

This works standalone — no global wind setting required (Fix #3). The per-instance values take effect whenever the project wind_strength is 0, via a shader fallback.

For scenes with *many* swaying splat instances sharing the same wind, set the project settings once (`ProjectSettings → rendering/gaussian_splatting/animation/wind_*`) and leave `wind_override_enabled = false` on each node — they'll inherit.

### Recipe D — Impact / shockwave pulse

Combine displacement + dissolve on a momentary effector. Spawn it at the impact point, animate its `radius` from 0 → big over ~0.3s, then free it.

```gdscript
# assumes: var impact_point: Vector3
var pulse := SphereEffector3D.new()
pulse.enabled = true
pulse.affect_position = true
pulse.strength = 2.5
pulse.falloff = 0.5
pulse.affect_opacity = true
pulse.target_opacity = 0.2
pulse.opacity_strength = 1.0
get_tree().current_scene.add_child(pulse)
pulse.global_position = impact_point

var tween := create_tween()
tween.tween_property(pulse, "radius", 8.0, 0.3).from(0.1)
tween.tween_callback(pulse.queue_free)
```

### Recipe E — Per-instance fade via `effect_*_scale` (no effector churn)

You want the effector stable, but the *individual splat* to ease in/out of the effect:

```gdscript
# assumes: var splat_node: GaussianSplatNode3D
# Tweens accept the full grouped property name as the property path.
var tween := create_tween()
tween.tween_property(splat_node, "rendering/effect_opacity_scale", 1.0, 0.5).from(0.0)
# Later (tweeners run in sequence):
tween.tween_property(splat_node, "rendering/effect_opacity_scale", 0.0, 0.5)
```

Cheaper than toggling `scene_effectors_enabled` — no generation bump, no cache invalidation.

### Recipe F — Exclusive group (this effector only affects props A & B)

Layer masks:

```gdscript
# assumes: var effector: SphereEffector3D; var prop_a: GaussianSplatNode3D; var prop_b: GaussianSplatNode3D
effector.layer_mask = 1 << 3   # bit 3
prop_a.set_scene_effector_layer_mask(1 << 3)
prop_b.set_scene_effector_layer_mask(1 << 3)
# Other props keep their default mask (1 = bit 0 only), so they do not overlap bit 3.
```

Effector won't match any splat whose layer mask doesn't overlap.

## Tuning cheat sheet

Reading a sphere effector's effect in your head:

| Knob | Effect on visible result |
|------|--------------------------|
| `radius` | World-space extent. Splat must be inside to be affected at all. |
| `falloff` | Shape of the rolloff. **0.3–0.5** = gentle, most of the sphere affected; **2.0** = quadratic, only the center region strongly affected; **≥4** = only the deep center. Start at 0.5 for dissolves, 1.0–2.0 for subtle position sway. |
| `strength` | Peak position amplitude in meters. Values >3 can push splats outside the visible prop silhouette. |
| `frequency` | Oscillation rate in Hz for position sway. **1–2 Hz** = wind-like; **>5 Hz** starts looking like trembling. Opacity path ignores this after the #1 fix — dissolve is a steady spatial envelope. |
| `target_opacity` | 0.0 = invisible inside the sphere; 1.0 = neutral (no change — API reports the effector inert). |
| `opacity_strength` | Per-frame push-toward-target. 1.0 = reach target fully; 0.5 = half-blend. |
| `effect_position_scale` (per-node) | Multiplier on how much *this* splat responds to position (default 1.0; inspector range 0..4, or greater). Keep it in 0..1 for sane game behavior. |
| `effect_opacity_scale` (per-node) | Same for opacity. |

**Falloff × position in sphere → approximate weight**: at a point `d` from the center with radius `r`, weight is `((r-d)/r)^falloff`. For a cabin 5 m from a 10 m sphere: `(0.5)^0.3 ≈ 0.81`, `(0.5)^2 ≈ 0.25`. That 3× difference is what makes the reference scene's moving-row feel "strong" vs "subtle".

## Debugging: is my effector actually doing anything?

Before blaming the system, check the runtime diagnostic:

```gdscript
# assumes: var splat_node: GaussianSplatNode3D
var state := splat_node.get_scene_effector_debug_state()
print("matched=", state.matched_count, " bound=", state.bound_count,
      " pos_active=", state.position_active, " op_active=", state.opacity_active,
      " effectors=", state.selected_effector_names)
```

Common outputs:

- `matched=0 bound=0` — filter mismatch. Check the ordered list in Part 1 § "My effector isn't affecting anything".
- `matched=N bound=4 truncated=true` — slot budget hit. Use `priority` or layer/scope filters to pick winners.
- `matched=1 bound=1 pos_active=false op_active=false` — an effector matched but produces no visible change. Most common cause: `target_opacity = 1.0` with only opacity enabled, or `strength = 0` with only position enabled.

Inspect the reference scene while running the demo binary to see what "working" looks like; your effector should match one of the configurations shown.

## Project-wide defaults (optional)

If most of your scene wants the same wind feel, set once in `ProjectSettings`:

- `rendering/gaussian_splatting/animation/wind_enabled = true`
- `rendering/gaussian_splatting/animation/wind_strength = 1.0`
- `rendering/gaussian_splatting/animation/wind_direction_x/y/z`
- `rendering/gaussian_splatting/animation/wind_frequency = 1.5`
- `rendering/gaussian_splatting/animation/wind_spatial_frequency = 0.1`
- `rendering/gaussian_splatting/animation/wind_time_scale = 1.0`

Then individual `GaussianSplatNode3D` nodes can leave `wind_override_enabled = false` and inherit, or override per-instance with any of the `rendering/wind_*` properties. Nodes that should explicitly *opt out* set `wind_override_enabled = true`, `wind_enabled = false`.

## When NOT to use this system

- You want to deform individual vertices / specific splats by index — the effector system is spatial only. Use a custom shader or animation clip.
- You want more than 4 simultaneous effects on one instance — budget is hard. Split content across instances or use `priority` carefully.
- You want exact physics-accurate splat motion — this is NPR / stylized only. No collision, no mass.
- You want cross-world effects — effectors are per-`World3D`. An effector in the main world won't touch splats in a subviewport world.

## Checklist before you ship a scene using effectors

- [ ] Each effector has `enabled = true`, finite transform, `radius > 0`.
- [ ] At least one of `affect_position` / `affect_opacity` is true, and the matching channel has a non-zero amplitude.
- [ ] Scope mode matches the scene hierarchy (default SUBTREE usually works; explicit root needed for mobile hierarchies).
- [ ] No more than 4 effectors match any single `GaussianSplatNode3D` (verify via `get_scene_effector_debug_state()`).
- [ ] If you need wind without project-level settings, `wind_override_enabled = true` on the nodes (Fix #3 lets the per-instance values stand alone).
- [ ] Don't bump `scene_effectors_enabled` / `scene_effector_layer_mask` every frame; use `effect_*_scale` for smooth transitions.

