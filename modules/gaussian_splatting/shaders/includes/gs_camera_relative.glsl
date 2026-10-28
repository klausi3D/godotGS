// gs_camera_relative.glsl -- the camera-relative frame of the HLOD route (package C2).
//
// With GS_HLOD_RELATIVE_PAYLOAD every position the depth and binning passes work with is
// relative to the camera, with world-aligned axes:
//
//     p_cam = R s p_node + camera_offset,   camera_offset = R s c + T - camera
//
// `p_node` is the splat position relative to its node centre (stored float32, shared by
// every instance and view), `camera_offset` is the per-draw value the CPU forms in double
// from the precise placement and the node's precise centre (DrawnNodeGPU.camera_offset).
// The instance translation T is part of camera_offset and is NOT added again here.
//
// Consequences, applied by the consumers:
//   * view transform: rotation only. The view matrix still carries its translation; a
//     camera-relative point is multiplied with w = GS_POSITION_FRAME_W = 0 so the
//     translation drops out (it is -R camera, already folded into p_cam).
//   * frustum planes: the CPU uploads planes of the camera-translated frustum (planes pass
//     through the camera); the plane test is unchanged.
//   * distance cull, SH view direction: the camera sits at the origin.
//   * wind phase and sphere effectors are defined in the scene frame; they receive the scene
//     frame camera position as `frame_anchor` (see gs_deformation.glsl).
// Without the define nothing here changes the frame: w = 1, the camera is
// `params.camera_position`, and the anchor is zero.

#ifndef GS_CAMERA_RELATIVE_GLSL
#define GS_CAMERA_RELATIVE_GLSL

#include "gs_instance_layout.glsl"

#if defined(GS_HLOD_RELATIVE_PAYLOAD)
#define GS_POSITION_FRAME_W 0.0
#else
#define GS_POSITION_FRAME_W 1.0
#endif

// Rotate `v` by the unit quaternion `q` (x, y, z, w). Same arithmetic as quat_rotate in
// depth_compute.glsl and gs_quat_rotate in tile_binning.glsl.
vec3 gs_cr_quat_rotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// Camera-relative position of a node-relative splat position.
vec3 gs_hlod_camera_relative_position(vec3 node_position, uint instance_flags, vec4 rotation,
        float uniform_scale, vec3 camera_offset) {
    vec3 p = node_position;
    if ((instance_flags & GS_INSTANCE_FLAG_SCALE_IDENTITY) == 0u) {
        p = p * uniform_scale;
    }
    if ((instance_flags & GS_INSTANCE_FLAG_ROTATION_IDENTITY) == 0u) {
        p = gs_cr_quat_rotate(rotation, p);
    }
    return p + camera_offset;
}

#endif // GS_CAMERA_RELATIVE_GLSL
