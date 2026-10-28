#ifndef GS_DEFORMATION_GLSL
#define GS_DEFORMATION_GLSL

#ifndef GS_MAX_SPHERE_EFFECTORS
#define GS_MAX_SPHERE_EFFECTORS 4
#endif

// Hash an instance identifier for wind phase variation.
uint gs_wind_hash_u32(uint v) {
    v ^= v >> 16u;
    v *= 0x7feb352du;
    v ^= v >> 15u;
    v *= 0x846ca68bu;
    v ^= v >> 16u;
    return v;
}

// Decode the per-instance wind mode from the packed float value.
uint gs_decode_instance_wind_mode(float encoded_mode) {
    return uint(clamp(floor(encoded_mode + 0.5), 0.0, float(GS_INSTANCE_WIND_MODE_FORCE_ENABLED)));
}

struct GSDeformationResult {
    vec3 position;
    float opacity;
};

uint gs_decode_instance_scene_effector_mask(vec4 instance_effect_config, vec4 effector_meta) {
    uint effector_count = min(uint(max(effector_meta.x, 0.0)), uint(GS_MAX_SPHERE_EFFECTORS));
    if (effector_count == 0u) {
        return 0u;
    }

    if (effector_meta.w <= 0.5) {
        return effector_count >= 32u ? 0xFFFFFFFFu : ((1u << effector_count) - 1u);
    }

    uint valid_mask = effector_count >= 32u ? 0xFFFFFFFFu : ((1u << effector_count) - 1u);
    return floatBitsToUint(instance_effect_config.z) & valid_mask;
}

// Check whether wind deformation is enabled for the current instance.
bool gs_is_wind_enabled_for_instance(float encoded_mode, vec4 wind_time_config) {
    bool wind_enabled = wind_time_config.w > 0.5;
    uint mode = gs_decode_instance_wind_mode(encoded_mode);
    if (mode == GS_INSTANCE_WIND_MODE_FORCE_ENABLED) {
        return true;
    }
    if (mode == GS_INSTANCE_WIND_MODE_FORCE_DISABLED) {
        return false;
    }
    return wind_enabled;
}

// Returns the steady spatial influence (0..1) for the point relative to the
// effector sphere. `out_anim_factor` carries the signed temporal pulse
// (-1..1) driven by `frequency`. Position deformation multiplies by it so
// splats oscillate *through* rest (wind-like sway) rather than breathing
// one-sidedly outward; opacity deformation ignores it so dissolve is a
// steady spatial envelope rather than a flicker at `frequency` Hz.
//
// The phase blends `time_seconds * frequency` with a low-frequency spatial
// term anchored to the effector center: this keeps neighboring splats
// correlated (coherent travelling wave across the sphere) instead of
// per-splat random phases that visually read as noise. The spatial
// coefficient is scaled by 1/radius so one full wavelength is comparable
// to the effector diameter — large enough that the whole influence region
// pulses together, small enough that there's still visible motion across
// the volume.
float gs_compute_sphere_effector_weight(vec3 world_position, vec4 effector_sphere, vec4 effector_config,
        float time_seconds, uint stable_seed, out vec3 out_direction, out float out_anim_factor) {
    out_direction = vec3(0.0, 1.0, 0.0);
    out_anim_factor = 0.0;
    if (effector_config.x <= 0.5) {
        return 0.0;
    }

    float radius = effector_sphere.w;
    if (radius <= 1e-6) {
        return 0.0;
    }

    vec3 delta = world_position - effector_sphere.xyz;
    float distance_to_center = length(delta);
    if (distance_to_center >= radius) {
        return 0.0;
    }

    float normalized = clamp(1.0 - (distance_to_center / radius), 0.0, 1.0);
    float falloff = max(effector_config.z, 0.001);
    float influence = pow(normalized, falloff);
    float anim_freq = effector_config.w > 0.0 ? effector_config.w : 2.0;
    // Spatial phase anchored to the effector center. Coefficient 1/radius
    // gives wavelength ≈ 2π·radius, so a travelling wave crosses the
    // sphere every few oscillations rather than standing-waving inside.
    float spatial_phase = dot(delta, vec3(1.0 / max(radius, 1e-3)));
    float anim_phase = time_seconds * anim_freq * 6.28318530718 + spatial_phase;
    out_anim_factor = sin(anim_phase);

    if (distance_to_center > 1e-6) {
        out_direction = delta / distance_to_center;
    }
    return influence;
}

GSDeformationResult gs_apply_sphere_effectors(vec3 world_position,
        float base_opacity,
        vec4 instance_effect_config,
        vec4 effector_meta,
        vec4 effector_spheres[GS_MAX_SPHERE_EFFECTORS],
        vec4 effector_configs[GS_MAX_SPHERE_EFFECTORS],
        vec4 effector_opacity_configs[GS_MAX_SPHERE_EFFECTORS],
        float time_seconds,
        uint stable_seed,
        vec3 frame_anchor) {
    GSDeformationResult result;
    result.position = world_position;
    result.opacity = clamp(base_opacity, 0.0, 1.0);

    uint effector_count = min(uint(max(effector_meta.x, 0.0)), uint(GS_MAX_SPHERE_EFFECTORS));
    if (effector_count == 0u) {
        return result;
    }

    float position_scale = max(instance_effect_config.x, 0.0);
    float opacity_scale = max(instance_effect_config.y, 0.0);
    uint instance_effector_mask = gs_decode_instance_scene_effector_mask(instance_effect_config, effector_meta);
    vec3 total_position_delta = vec3(0.0);
    float opacity_modifier_product = 1.0;

    for (uint i = 0u; i < effector_count; i++) {
        // Spheres are scene-frame values; `world_position` is relative to `frame_anchor`
        // (zero outside the HLOD camera-relative route, where the subtraction is exact).
        vec4 effector_sphere = effector_spheres[i];
        effector_sphere.xyz -= frame_anchor;
        vec4 effector_config = effector_configs[i];
        vec4 effector_opacity_config = effector_opacity_configs[i];
        if (effector_config.x <= 0.5 || effector_sphere.w <= 1e-6) {
            continue;
        }
        if (effector_meta.w > 0.5 && (instance_effector_mask & (1u << i)) == 0u) {
            continue;
        }

        vec3 direction;
        float anim_factor;
        float weight = gs_compute_sphere_effector_weight(world_position, effector_sphere, effector_config,
                time_seconds, stable_seed + i * 0x9e3779b9u, direction, anim_factor);
        if (weight <= 0.0) {
            continue;
        }

        float strength = effector_config.y;
        if (effector_opacity_config.x > 0.5 && position_scale > 0.0 && abs(strength) > 1e-8) {
            // Position deformation carries the temporal pulse so motion feels
            // wind-like. Opacity deliberately omits `anim_factor` below.
            total_position_delta += direction * (strength * weight * anim_factor * position_scale);
        }

        if (effector_opacity_config.y > 0.5 && opacity_scale > 0.0) {
            float opacity_strength = clamp(effector_opacity_config.z, 0.0, 1.0);
            float response_weight = clamp(opacity_strength * opacity_scale, 0.0, 1.0);
            if (response_weight > 0.0) {
                float target_opacity = clamp(effector_opacity_config.w, 0.0, 1.0);
                float desired = mix(1.0, target_opacity, clamp(weight, 0.0, 1.0));
                float modifier = mix(1.0, desired, response_weight);
                opacity_modifier_product *= clamp(modifier, 0.0, 1.0);
            }
        }
    }

    result.position += total_position_delta;
    result.opacity = clamp(result.opacity * opacity_modifier_product, 0.0, 1.0);

    return result;
}

GSDeformationResult gs_apply_wind_deformation(vec3 world_position,
        uint stable_seed,
        float opacity,
        float instance_intensity,
        float instance_wind_mode,
        vec4 instance_wind_config,
        vec4 instance_effect_config,
        vec4 wind_dir_strength,
        vec4 wind_time_config,
        vec4 effector_meta,
        vec4 effector_spheres[GS_MAX_SPHERE_EFFECTORS],
        vec4 effector_configs[GS_MAX_SPHERE_EFFECTORS],
        vec4 effector_opacity_configs[GS_MAX_SPHERE_EFFECTORS],
        vec3 frame_anchor) {
    float time_seconds = wind_time_config.x;
    bool wind_enabled = gs_is_wind_enabled_for_instance(instance_wind_mode, wind_time_config);
    if (!wind_enabled) {
        return gs_apply_sphere_effectors(world_position, opacity, instance_effect_config, effector_meta,
                effector_spheres, effector_configs, effector_opacity_configs, time_seconds, stable_seed, frame_anchor);
    }

    vec3 direction = wind_dir_strength.xyz;
    if (dot(instance_wind_config.xyz, instance_wind_config.xyz) > 1e-8) {
        direction = instance_wind_config.xyz;
    }
    float direction_len = length(direction);
    float strength = max(wind_dir_strength.w, 0.0);
    float base_temporal_frequency = max(wind_time_config.y, 0.0);
    // Per-instance wind override (rendering/wind_override_enabled + wind_enabled
    // on the node) bypasses the global enable gate but, without these fallbacks,
    // is still muted by zero project-level wind_strength / wind_frequency. Since
    // wind_dir_strength.w is a pass-global uniform, we can't merge a per-instance
    // value into it on the CPU for mixed scenes — instead, when the instance
    // explicitly forces wind on and the global base is unset, fall back to unit
    // strength / frequency here so the instance's own wind_strength (multiplied
    // into instance_intensity) and wind_frequency (instance_wind_config.w)
    // become effective.
    uint mode = gs_decode_instance_wind_mode(instance_wind_mode);
    if (mode == GS_INSTANCE_WIND_MODE_FORCE_ENABLED) {
        if (strength <= 0.0) {
            strength = 1.0;
        }
        if (base_temporal_frequency <= 0.0) {
            base_temporal_frequency = 1.0;
        }
    }
    if (direction_len <= 1e-6 || strength <= 0.0) {
        return gs_apply_sphere_effectors(world_position, opacity, instance_effect_config, effector_meta,
                effector_spheres, effector_configs, effector_opacity_configs, time_seconds, stable_seed, frame_anchor);
    }

    float instance_frequency_scale = instance_wind_config.w > 0.0 ? instance_wind_config.w : 1.0;
    float temporal_frequency = base_temporal_frequency * instance_frequency_scale;
    float spatial_frequency = wind_time_config.z;
    float clamped_intensity = max(instance_intensity, 0.0);
    if (clamped_intensity <= 0.0) {
        return gs_apply_sphere_effectors(world_position, opacity, instance_effect_config, effector_meta,
                effector_spheres, effector_configs, effector_opacity_configs, time_seconds, stable_seed, frame_anchor);
    }

    // Reuse opacity as a rough "stiffness" signal until a dedicated attribute exists.
    float opacity_resistance = clamp(1.0 - opacity, 0.0, 1.0);
    float resistance = mix(0.2, 1.0, opacity_resistance);

    uint hashed = gs_wind_hash_u32(stable_seed);
    float phase_jitter = (float(hashed & 0xFFFFu) / 65535.0) * 6.28318530718;
    // The wind phase is a function of the scene-frame position, not of the camera-relative one.
    float phase = dot((world_position + frame_anchor).xz, vec2(spatial_frequency)) + time_seconds * temporal_frequency + phase_jitter;
    float displacement = sin(phase) * strength * resistance * clamped_intensity;

    vec3 deformed = world_position + (direction / direction_len) * displacement;
    return gs_apply_sphere_effectors(deformed, opacity, instance_effect_config, effector_meta,
            effector_spheres, effector_configs, effector_opacity_configs, time_seconds, stable_seed, frame_anchor);
}

#endif // GS_DEFORMATION_GLSL
