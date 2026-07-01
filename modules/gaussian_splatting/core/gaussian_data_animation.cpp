/**
 * gaussian_data_animation.cpp -- Companion .cpp for gaussian_data.h
 *
 * Contains the animation system integration methods of GaussianData:
 *   - Animation state machine / incremental saver wiring
 *   - Per-frame animation update and cache management
 *   - Per-splat animated property accessors (position, color, opacity,
 *     scale, rotation)
 */

#include "gaussian_data.h"

// ---------------------------------------------------------------------------
// Animation state-machine & incremental-saver wiring
// ---------------------------------------------------------------------------

void GaussianData::set_animation_state_machine(const Ref<GaussianSplatting::GaussianAnimationStateMachine>& p_animation) {
    {
        RWLockWrite lock(data_rwlock);
        animation_state_machine = p_animation;
        MutexLock anim_lock(animation_cache_mutex);
        if (animation_state_machine.is_valid()) {
            animation_state_machine->set_splat_count(gaussians.size());
            if (incremental_saver.is_valid()) {
                animation_state_machine->set_incremental_saver(incremental_saver);
            }
        }
    }
}

Ref<GaussianSplatting::GaussianAnimationStateMachine> GaussianData::get_animation_state_machine() const {
    RWLockRead lock(data_rwlock);
    return animation_state_machine;
}

bool GaussianData::has_animation() const {
    RWLockRead lock(data_rwlock);
    return animation_state_machine.is_valid();
}

void GaussianData::set_incremental_saver(const Ref<GaussianSplatting::GaussianIncrementalSaver>& p_saver) {
    RWLockWrite lock(data_rwlock);
    incremental_saver = p_saver;
    MutexLock anim_lock(animation_cache_mutex);
    if (incremental_saver.is_valid() && animation_state_machine.is_valid()) {
        animation_state_machine->set_incremental_saver(incremental_saver);
    }
}

Ref<GaussianSplatting::GaussianIncrementalSaver> GaussianData::get_incremental_saver() const {
    RWLockRead lock(data_rwlock);
    return incremental_saver;
}

// ---------------------------------------------------------------------------
// Per-frame animation update
// ---------------------------------------------------------------------------

void GaussianData::update_animation(float p_delta) {
    Ref<GaussianSplatting::GaussianAnimationStateMachine> animation;
    bool enabled = false;
    {
        RWLockRead lock(data_rwlock);
        animation = animation_state_machine;
        enabled = animation_enabled;
    }

    if (!animation.is_valid() || !enabled) {
        return;
    }

    {
        MutexLock anim_lock(animation_cache_mutex);
        animation->update(p_delta);
    }
}

void GaussianData::set_animation_enabled(bool p_enabled) {
    {
        RWLockWrite lock(data_rwlock);
        animation_enabled = p_enabled;
    }
}

bool GaussianData::is_animation_enabled() const {
    RWLockRead lock(data_rwlock);
    return animation_enabled;
}

// ---------------------------------------------------------------------------
// Non-destructive animation view.
//
// The prior apply_animation_at_time() destructively wrote sampled animation
// values back into the base Gaussian payload. That collapsed the distinction
// between base state and the animated view and made GaussianData's mutable
// runtime truth time-dependent. It has been removed -- animation is now
// strictly a sampling view, exposed through the per-property accessors
// below. Consumers that want a baked pose must construct a new GaussianData
// and stamp the sampled values into it themselves.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Per-splat animated property accessors
// ---------------------------------------------------------------------------

Vector3 GaussianData::get_animated_position(int p_index, float p_time) const {
    Vector3 base_position;
    Ref<GaussianSplatting::GaussianAnimationStateMachine> animation;
    bool enabled = false;
    {
        RWLockRead lock(data_rwlock);
        if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
            return Vector3();
        }
        base_position = gaussians[p_index].position;
        animation = animation_state_machine;
        enabled = animation_enabled;
    }

    if (!animation.is_valid() || !enabled) {
        return base_position;
    }

    Vector3 animated_pos;
    MutexLock anim_lock(animation_cache_mutex);
    if (animation->try_sample_position(p_index, p_time, animated_pos)) {
        return animated_pos;
    }

    return base_position;
}

Color GaussianData::get_animated_color(int p_index, float p_time) const {
    Color base_color;
    Ref<GaussianSplatting::GaussianAnimationStateMachine> animation;
    bool enabled = false;
    {
        RWLockRead lock(data_rwlock);
        if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
            return Color();
        }
        base_color = gaussians[p_index].sh_dc;
        animation = animation_state_machine;
        enabled = animation_enabled;
    }

    if (!animation.is_valid() || !enabled) {
        return base_color;
    }

    Color animated_color;
    MutexLock anim_lock(animation_cache_mutex);
    if (animation->try_sample_color(p_index, p_time, animated_color)) {
        return animated_color;
    }

    return base_color;
}

float GaussianData::get_animated_opacity(int p_index, float p_time) const {
    float base_opacity = 1.0f;
    Ref<GaussianSplatting::GaussianAnimationStateMachine> animation;
    bool enabled = false;
    {
        RWLockRead lock(data_rwlock);
        if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
            return 1.0f;
        }
        base_opacity = gaussians[p_index].opacity;
        animation = animation_state_machine;
        enabled = animation_enabled;
    }

    if (!animation.is_valid() || !enabled) {
        return base_opacity;
    }

    float animated_opacity;
    MutexLock anim_lock(animation_cache_mutex);
    if (animation->try_sample_opacity(p_index, p_time, animated_opacity)) {
        return animated_opacity;
    }

    return base_opacity;
}

Vector3 GaussianData::get_animated_scale(int p_index, float p_time) const {
    Vector3 base_scale(1, 1, 1);
    Ref<GaussianSplatting::GaussianAnimationStateMachine> animation;
    bool enabled = false;
    {
        RWLockRead lock(data_rwlock);
        if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
            return Vector3(1, 1, 1);
        }
        base_scale = gaussians[p_index].scale;
        animation = animation_state_machine;
        enabled = animation_enabled;
    }

    if (!animation.is_valid() || !enabled) {
        return base_scale;
    }

    Vector3 animated_scale;
    MutexLock anim_lock(animation_cache_mutex);
    if (animation->try_sample_scale(p_index, p_time, animated_scale)) {
        return animated_scale;
    }

    return base_scale;
}

Quaternion GaussianData::get_animated_rotation(int p_index, float p_time) const {
    Quaternion base_rotation;
    Ref<GaussianSplatting::GaussianAnimationStateMachine> animation;
    bool enabled = false;
    {
        RWLockRead lock(data_rwlock);
        if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
            return Quaternion();
        }
        base_rotation = gaussians[p_index].rotation;
        animation = animation_state_machine;
        enabled = animation_enabled;
    }

    if (!animation.is_valid() || !enabled) {
        return base_rotation;
    }

    Quaternion animated_rotation;
    MutexLock anim_lock(animation_cache_mutex);
    if (animation->try_sample_rotation(p_index, p_time, animated_rotation)) {
        return animated_rotation;
    }

    return base_rotation;
}
