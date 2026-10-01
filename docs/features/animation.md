# Animation

## Purpose

Describe per-splat property values (position, color, opacity, scale, rotation) over time with `GaussianAnimationStateMachine`. The class provides clips, keyframe interpolation, and playback control, and returns sampled values to your script.

!!! warning "Scope and limitations"
    - **Opt-in.** `GaussianAnimationStateMachine` is a `Resource` that the engine never creates for you. Animation exists only when your project creates one and attaches it with `GaussianData.set_animation_state_machine()`. By default `GaussianData.get_animation_state_machine()` returns `null` and `GaussianData.has_animation()` returns `false`. (`animation_state_machine` is not a property; use the getter and setter.)
    - **CPU sampling only; nothing renders it.** No part of the renderer, the nodes, or the shaders reads the state machine. Its only consumers are `GaussianData`'s `get_animated_*()` accessors and the scene serializer. Sampling returns values to your script; attaching an animation to the `GaussianData` shown by a `GaussianSplatNode3D` does not make the splats move on screen.
    - **No `AnimationPlayer` integration.** The state machine exposes no animatable properties and has no tracks in Godot's animation system. Advance it by calling `update(delta)` yourself.
    - **No cross-fade.** Clip switching is a delayed hard cut (see [Switching between clips](#switching-between-clips)).

## Animation system overview

`GaussianAnimationStateMachine` is a `Resource`-derived class that holds animation clips, tracks, and keyframes. Each clip contains one or more tracks, and each track targets a specific `AnimationProperty`. Keyframes store timed values with an interpolation type. A keyframe value is either one value for every splat, or a packed array with one entry per splat (for example a `PackedVector3Array` for positions).

| Concept | Description | Implementation reference |
| --- | --- | --- |
| AnimationClip | Named container with a duration, loop flag, and a set of tracks. | `GaussianSplatting::AnimationClip` |
| AnimationTrack | Targets one `AnimationProperty` and holds a sorted list of keyframes. | `GaussianSplatting::AnimationTrack` |
| AnimationProperty | Enum: `ANIMATION_PROPERTY_POSITION`, `_COLOR`, `_OPACITY`, `_SCALE`, `_ROTATION`. | `GaussianSplatting::AnimationProperty` |
| AnimationState | Enum: `ANIMATION_STATE_STOPPED`, `_PLAYING`, `_PAUSED`, `_SEEKING`. | `GaussianSplatting::AnimationState` |
| KeyframeInterpolator | Interpolates between keyframes with constant, linear, cubic Bezier, smooth-step, and smoother-step curves. | `GaussianSplatting::KeyframeInterpolator`, `GaussianSplatting::InterpolationType` |

## Setting up animation clips

### Create a clip and add tracks

This is a complete script. Attach it to a `Node` and point the path at a PLY in your project.

!!! note "Pass `AnimationProperty` values through `int()`"
    The property enum is declared at namespace scope in C++ (`GaussianSplatting::AnimationProperty`) but its constants are bound on the class. The GDScript analyzer therefore types `GaussianAnimationStateMachine.ANIMATION_PROPERTY_POSITION` and the method parameter as two different enums and rejects a direct pass with a parse error (*Cannot pass a value of type "GaussianAnimationStateMachine.AnimationProperty" as "GaussianSplatting.AnimationProperty"*). Wrapping the constant in `int()` compiles and works. This is a binding defect, not intended usage; it is tracked in [#1106](https://github.com/klausi3D/godotGS/issues/1106), and once that is fixed the `int()` wrapper is no longer needed.

```gdscript
extends Node

const POSITION := int(GaussianAnimationStateMachine.ANIMATION_PROPERTY_POSITION)
const OPACITY := int(GaussianAnimationStateMachine.ANIMATION_PROPERTY_OPACITY)

var data := GaussianData.new()
var anim := GaussianAnimationStateMachine.new()
var clip_idx := -1

func _ready() -> void:
	var err := data.load_from_file("res://models/scan.ply")
	if err != OK:
		push_error("load_from_file failed: %s" % error_string(err))
		return

	anim.set_splat_count(data.get_count())

	# Create a 2-second looping clip named "wobble".
	clip_idx = anim.add_clip("wobble", 2.0)
	anim.set_clip_looping(clip_idx, true)

	# Add a position track and an opacity track.
	anim.add_track_to_clip(clip_idx, POSITION)
	anim.add_track_to_clip(clip_idx, OPACITY)

	# One value for every splat, linear interpolation.
	anim.add_keyframe(clip_idx, POSITION, 0.0, Vector3(0, 0, 0))
	anim.add_keyframe(clip_idx, POSITION, 1.0, Vector3(0, 1, 0))
	anim.add_keyframe(clip_idx, POSITION, 2.0, Vector3(0, 0, 0))

	# Cubic Bezier keyframes for the opacity track.
	anim.add_keyframe_bezier(clip_idx, OPACITY, 0.0, 1.0, Vector2(0.0, 0.0), Vector2(0.3, 0.0))
	anim.add_keyframe_bezier(clip_idx, OPACITY, 1.0, 0.5, Vector2(-0.3, 0.0), Vector2(0.3, 0.0))
	anim.add_keyframe_bezier(clip_idx, OPACITY, 2.0, 1.0, Vector2(-0.3, 0.0), Vector2(0.0, 0.0))

	data.set_animation_state_machine(anim)
	anim.play(clip_idx)

func _process(delta: float) -> void:
	if not anim.is_playing():
		return
	anim.update(delta)
	# Sampled values are returned to the script; they are not rendered automatically.
	var pos := anim.sample_position(0)
	var opacity := anim.sample_opacity(0)
	if Engine.get_process_frames() % 60 == 0:
		print("t=%.2f splat0 position=%s opacity=%.2f" % [anim.get_current_time(), pos, opacity])
```

| Method | Description | Implementation reference |
| --- | --- | --- |
| `add_clip(name, duration)` | Creates a new clip and returns its index, or `-1` (with an error) if the name already exists. | `GaussianAnimationStateMachine::add_clip` |
| `set_clip_duration(index, duration)` | Changes the clip duration. | `GaussianAnimationStateMachine::set_clip_duration` |
| `set_clip_looping(index, looping)` | Enables or disables looping. | `GaussianAnimationStateMachine::set_clip_looping` |
| `add_track_to_clip(clip_index, property)` | Adds a track for the given property. | `GaussianAnimationStateMachine::add_track_to_clip` |
| `remove_track_from_clip(clip_index, property)` | Removes the track for the given property. | `GaussianAnimationStateMachine::remove_track_from_clip` |
| `has_track(clip_index, property)` | Returns whether the clip has a track for the property. | `GaussianAnimationStateMachine::has_track` |
| `set_splat_count(count)` | Sets how many splat indices can be sampled. `GaussianData` also updates it when data attached to it is resized. | `GaussianAnimationStateMachine::set_splat_count` |

### Keyframes

| Method | Description | Implementation reference |
| --- | --- | --- |
| `add_keyframe(clip_index, property, time, value)` | Inserts a keyframe with linear interpolation. | `GaussianAnimationStateMachine::add_keyframe` |
| `add_keyframe_bezier(clip_index, property, time, value, in_handle, out_handle)` | Inserts a keyframe with cubic Bezier interpolation handles. | `GaussianAnimationStateMachine::add_keyframe_bezier` |
| `remove_keyframe(clip_index, property, keyframe_index)` | Removes a keyframe by index. | `GaussianAnimationStateMachine::remove_keyframe` |
| `get_keyframe_count(clip_index, property)` | Returns the number of keyframes on a track. | `GaussianAnimationStateMachine::get_keyframe_count` |
| `get_keyframe_time(clip_index, property, keyframe_index)` | Returns the time of a specific keyframe. | `GaussianAnimationStateMachine::get_keyframe_time` |
| `get_keyframe_value(clip_index, property, keyframe_index)` | Returns the value stored in a keyframe. | `GaussianAnimationStateMachine::get_keyframe_value` |

### Interpolation types

Keyframe interpolation is determined by the `InterpolationType` stored with each keyframe. From GDScript only `LINEAR` (`add_keyframe()`) and `CUBIC_BEZIER` (`add_keyframe_bezier()`) can be created; the other types are reachable from C++ only.

| Type | Behavior | Implementation reference |
| --- | --- | --- |
| `CONSTANT` | Holds the keyframe value until the next keyframe. | `GaussianSplatting::InterpolationType` |
| `LINEAR` | Linear interpolation (`lerp` for scalars/vectors, `slerp` for quaternions). | `KeyframeInterpolator::_interpolate_vector3` |
| `CUBIC_BEZIER` | Cubic Bezier curve using in/out handles. | `KeyframeInterpolator::_cubic_bezier` |
| `SMOOTH_STEP` | Hermite smooth-step: `t*t*(3-2t)`. | `KeyframeInterpolator::smooth_step` |
| `SMOOTHER_STEP` | Ken Perlin smoother-step: `t*t*t*(t*(6t-15)+10)`. | `KeyframeInterpolator::smoother_step` |

## Playback and blending

### Playback control

| Method | Description | Implementation reference |
| --- | --- | --- |
| `play(clip_index)` | Starts playback. Pass `-1` to resume the current clip. | `GaussianAnimationStateMachine::play` |
| `pause()` | Pauses playback at the current time. | `GaussianAnimationStateMachine::pause` |
| `stop()` | Stops playback and resets to the beginning. | `GaussianAnimationStateMachine::stop` |
| `seek(time)` | Jumps to a specific time in the current clip. Negative times clamp to `0`; a non-finite time is rejected with a warning (see below). | `GaussianAnimationStateMachine::seek` |
| `set_playback_speed(speed)` | Changes the playback rate multiplier. A non-finite speed is rejected with a warning (see below). | `GaussianAnimationStateMachine::set_playback_speed` |
| `update(delta)` | Advances the animation by `delta` seconds. Call this every frame. A non-finite `delta` skips the frame (see below). | `GaussianAnimationStateMachine::update` |
| `is_playing()` | Returns `true` when the state machine is in the `PLAYING` state. | `GaussianAnimationStateMachine::is_playing` |
| `get_current_time()` | Returns the current playback position in seconds. | `GaussianAnimationStateMachine::get_current_time` |

`GaussianData.update_animation(delta)` forwards to `update(delta)` when an animation is attached and animation is enabled on the `GaussianData` (`animation_enabled`, default `true`; turn it off with `set_animation_enabled(false)`).

#### Non-finite playback values are rejected

`NAN`, `INF` and `-INF` are rejected at the point they enter the state machine,
because a non-finite time or speed makes *every* later comparison against it
false (`NaN < x`, `NaN > x` and `NaN == x` are all false). Playback would then
never wrap and never stop: it freezes silently, with no crash and no error.

| Input | Behaviour |
| --- | --- |
| `seek(non-finite)` | Warns, seeks to `0` instead. |
| `set_playback_speed(non-finite)` | Warns, uses `1.0` instead. |
| `update(non-finite)` | Warns, skips the frame; the current time is left untouched. |
| Loading a saved animation (C++ `from_dict()`) with a non-finite `current_time` / `playback_speed` | Warns, substitutes `0` / `1.0`. Applies to loading a corrupt or hand-edited save. |

`play()` deliberately does **not** reset the current time, so a `seek()` is
honoured by a following `play()`; `stop()` is what resets to `0`.

A finite *negative* `set_playback_speed()` is accepted, but true reverse
playback is not implemented — the current time is floored at `0` rather than
wrapping backwards.

### Switching between clips

> **Note:** cross-fade blending is not yet implemented. `switch_to_clip_delayed()`
> schedules a *delayed hard cut*: until the delay elapses the current clip keeps
> playing unchanged, then playback jumps to the target clip in a single step. The
> target clip is never sampled or weighted during the delay. `set_clip_weight()` /
> `get_clip_weight()` track a weight value that no sampler currently reads. Real
> weighted cross-fade sampling is tracked as a follow-up (godotGS issue #614).

```gdscript
# assumes: var anim: GaussianAnimationStateMachine
# Create a second clip.
var idle_idx := anim.add_clip("idle", 3.0)
anim.set_clip_looping(idle_idx, true)
var position_property := int(GaussianAnimationStateMachine.ANIMATION_PROPERTY_POSITION)
anim.add_track_to_clip(idle_idx, position_property)
anim.add_keyframe(idle_idx, position_property, 0.0, Vector3.ZERO)

# Switch to "idle" after 0.5 seconds (delayed hard cut, not a cross-fade).
anim.switch_to_clip_delayed(idle_idx, 0.5)
```

| Method | Description | Implementation reference |
| --- | --- | --- |
| `switch_to_clip_delayed(clip_index, switch_delay)` | Schedules a delayed hard switch to the target clip after `switch_delay` seconds (default 0.3). Not a cross-fade. | `GaussianAnimationStateMachine::switch_to_clip_delayed` |
| `blend_to_clip(clip_index, blend_duration)` | **Deprecated** alias for `switch_to_clip_delayed()`; the name implied a cross-fade that is not implemented. | `GaussianAnimationStateMachine::blend_to_clip` |
| `set_clip_weight(clip_index, weight)` | Sets a clip's blend weight. Stored but not yet consumed by any sampler. | `GaussianAnimationStateMachine::set_clip_weight` |
| `get_clip_weight(clip_index)` | Returns the current stored blend weight. | `GaussianAnimationStateMachine::get_clip_weight` |

### Sampling

Sample animated property values of the **current clip** at the current playback time (pass `time = -1`, the default) or at an explicit time. When the splat index is out of range, no clip is current, or the track is missing or empty, the sampler returns a fallback: `Vector3.ZERO` for position, `Color()` for color, `1.0` for opacity, `Vector3.ONE` for scale.

| Method | Returns | Description | Implementation reference |
| --- | --- | --- | --- |
| `sample_position(splat_index, time)` | `Vector3` | Position at the given time (default: current time). | `GaussianAnimationStateMachine::sample_position` |
| `sample_color(splat_index, time)` | `Color` | Color at the given time. | `GaussianAnimationStateMachine::sample_color` |
| `sample_opacity(splat_index, time)` | `float` | Opacity at the given time. | `GaussianAnimationStateMachine::sample_opacity` |
| `sample_scale(splat_index, time)` | `Vector3` | Scale at the given time. | `GaussianAnimationStateMachine::sample_scale` |
| `sample_rotation(splat_index, time)` | `Quaternion` | Rotation at the given time. | `GaussianAnimationStateMachine::sample_rotation` |

`GaussianData.get_animated_position(index, time)` (and `_color`, `_opacity`, `_scale`, `_rotation`) samples the attached state machine through the `GaussianData` instead.

### Saving and loading

`GaussianAnimationStateMachine::to_dict()` and `from_dict()` exist in C++ only; they are not bound, so `anim.to_dict()` fails in GDScript. The class also exposes no stored properties, so `ResourceSaver.save()` on a state machine does not keep its clips.

The scriptable way to persist an animation is the scene serializer, which writes the splat data and the animation into one `.gsf` file:

```gdscript
# assumes: var data: GaussianData; var anim: GaussianAnimationStateMachine
var serializer := GaussianSceneSerializer.new()
var err := serializer.save_scene("user://animated_scene.gsf", data, anim)
if err != OK:
	push_error("save_scene failed: %s" % error_string(err))

# Later: load into fresh objects.
var loaded_data := GaussianData.new()
var loaded_anim := GaussianAnimationStateMachine.new()
err = serializer.load_scene("user://animated_scene.gsf", loaded_data, loaded_anim)
if err == OK:
	print("clips restored: ", loaded_anim.get_clip_count())
```

## Troubleshooting

| Symptom | Cause | Fix | Implementation reference |
| --- | --- | --- | --- |
| `add_clip` returns `-1` and logs an error | A clip with that name already exists. | Use a unique name or remove the existing clip with `remove_clip_by_name()`. | `GaussianAnimationStateMachine::add_clip` |
| Sampled values do not change over time | `update(delta)` is not being called each frame, or playback was never started. | Call `anim.play(clip_idx)` once and `anim.update(delta)` in `_process`. | `GaussianAnimationStateMachine::update` |
| Splats do not move on screen | Expected: the renderer does not read animation data. | Use the sampled values in your own code. | `GaussianData::get_animated_position` |
| Rotation interpolation produces unexpected results | Keyframe values are not normalized quaternions. | Provide normalized `Quaternion` values; the interpolator uses `slerp`. | `KeyframeInterpolator::_interpolate_quaternion` |
| Clip switch looks like a hard cut | Expected: cross-fade is not implemented (#614). | None yet. | `GaussianAnimationStateMachine::switch_to_clip_delayed` |
| `sample_position` returns `Vector3.ZERO` | No clip is current, the splat index is out of range, or no position track has keyframes. | Call `play()`, check `set_splat_count()`, and add a position track with keyframes. | `GaussianAnimationStateMachine::sample_position` |
