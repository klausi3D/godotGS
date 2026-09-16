#ifndef GS_SCENE_DEPTH_GUARD_GLSL
#define GS_SCENE_DEPTH_GUARD_GLSL

// Shared scene-depth occlusion guard for the Gaussian composites.
//
// Both composites -- the compute blit (viewport_blit.glsl) and the painterly
// viewport composite (painterly_composite.glsl) -- must decide the same thing:
// "is this splat fragment behind opaque scene geometry?". They used to answer it
// with two independently written implementations. The painterly one linearized
// scene depth from the RAW camera projection columns
// (abs(proj_32 / (proj_22 + raw))) instead of the depth-corrected pair the host
// derives in interfaces/gs_scene_depth_linearize.h, so it never recognised the
// background clear value and discarded EVERY splat fragment. That was invisible
// for as long as the painterly composite itself was unreachable (#986); the
// moment it started running, painterly rendered nothing.
//
// The host side already has exactly one derivation
// (gs_derive_scene_depth_linearize); this is its shader-side counterpart. Every
// input is an explicit argument -- no `params` block is referenced -- so the two
// callers can keep their own push-constant layouts while sharing the math.
//
// Depth convention: `raw_scene_depth` is the scene depth buffer sample and
// `gs_depth` is the splat renderer's NORMALIZED linear depth, which both
// composites expand as mix(z_near, z_far, gs_depth).

// Raw scene depth -> positive view-space z.
float gs_linearize_scene_depth(float raw_depth, int is_orthogonal, float z_near, float z_far,
        float linearize_mul, float linearize_add) {
    if (is_orthogonal != 0) {
        float ndc = raw_depth * 2.0 - 1.0;
        return -(ndc * (z_far - z_near) - (z_far + z_near)) / 2.0;
    }
    return linearize_mul / (linearize_add - raw_depth);
}

// Clamp invalid depth values to a sentinel the callers test with `>= 0.0`.
float gs_sanitize_view_depth(float depth_value) {
    if (isnan(depth_value) || isinf(depth_value)) {
        return -1.0;
    }
    return abs(depth_value);
}

// Does this scene-depth sample correspond to the background clear value?
// Derived from the projection rather than assumed, so it holds for both depth
// directions (reverse-Z included): linearize raw 0.0 and raw 1.0, take the
// farther of the two as the far plane, and its raw value as the clear value.
bool gs_is_scene_background_depth(float raw_scene_depth, float scene_view_depth,
        int is_orthogonal, float z_near, float z_far, float linearize_mul, float linearize_add,
        float depth_epsilon) {
    float from_zero = gs_sanitize_view_depth(
            gs_linearize_scene_depth(0.0, is_orthogonal, z_near, z_far, linearize_mul, linearize_add));
    float from_one = gs_sanitize_view_depth(
            gs_linearize_scene_depth(1.0, is_orthogonal, z_near, z_far, linearize_mul, linearize_add));
    if (from_zero < 0.0 || from_one < 0.0) {
        return false;
    }

    float clear_raw_depth = from_zero >= from_one ? 0.0 : 1.0;
    float far_view_depth = max(from_zero, from_one);
    float far_tolerance = max(depth_epsilon * 2.0, 1e-3);

    bool raw_matches_clear = abs(raw_scene_depth - clear_raw_depth) <= 1e-6;
    bool view_matches_far = abs(scene_view_depth - far_view_depth) <= far_tolerance;
    return raw_matches_clear || view_matches_far;
}

// True when the splat fragment is occluded by opaque scene geometry and must be
// dropped. False for background, for out-of-range depths, and for splats at the
// far plane -- the guard only ever REMOVES fragments it can prove are behind
// real geometry, so an unusable depth pair composites rather than blanks.
bool gs_scene_depth_occludes(float raw_scene_depth, float gs_depth, int is_orthogonal,
        float z_near, float z_far, float linearize_mul, float linearize_add, float depth_epsilon) {
    bool depths_in_range = (gs_depth >= 0.0 && gs_depth <= 1.0 &&
            raw_scene_depth >= 0.0 && raw_scene_depth <= 1.0);
    if (!depths_in_range || gs_depth >= 0.999999) {
        return false;
    }

    float scene_view_depth = gs_sanitize_view_depth(
            gs_linearize_scene_depth(raw_scene_depth, is_orthogonal, z_near, z_far, linearize_mul, linearize_add));
    float gs_view_depth = gs_sanitize_view_depth(mix(z_near, z_far, clamp(gs_depth, 0.0, 1.0)));

    if (gs_is_scene_background_depth(raw_scene_depth, scene_view_depth, is_orthogonal,
            z_near, z_far, linearize_mul, linearize_add, depth_epsilon)) {
        return false;
    }

    return scene_view_depth >= 0.0 && gs_view_depth >= 0.0 &&
            scene_view_depth <= gs_view_depth - depth_epsilon;
}

#endif // GS_SCENE_DEPTH_GUARD_GLSL
