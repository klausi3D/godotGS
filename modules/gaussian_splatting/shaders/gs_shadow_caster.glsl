// gs_shadow_caster.glsl -- depth-only splat shadow caster for directional cascades on the HLOD route (#1095, #1163).
//
// One instanced quad per splat slot of the main view's HLOD draw list (DrawnNodeGPU[]): slot = record * max_chunk_splats
// + splat, the depth pass's 2D dispatch flattened (compute/depth_compute.glsl). Slots past a record's payload emit a
// degenerate quad. Positions are camera-relative (gs_camera_relative.glsl); light_from_frame folds the frame camera back
// in, so the clip transform is exactly Godot's directional shadow transform (correction * P * V, reverse-Z), and the
// pancake clamp matches Godot's mesh casters. The hardware depth test (GREATER_OR_EQUAL) keeps the nearest occluder; a
// fragment occludes when its Gaussian alpha reaches the threshold (renderer/hlod_shadow_caster_plan.h). No sort.

#[vertex]

#version 450

#include "includes/gs_instance_layout.glsl"
#include "includes/gs_camera_relative.glsl"

// Raw relative payload; same layout as PackedGaussian in compute/depth_compute.glsl and Gaussian in tile_binning.glsl.
struct CasterGaussian {
    vec3 position;
    float opacity;
    vec3 scale;
    uint _pad_rotation_align;
    vec4 rotation;
    vec4 sh_dc;
    uint sh_encoded[24];
    vec3 normal;
    uint sh_metadata;
};

layout(set = 0, binding = 0, std430) readonly buffer AtlasGaussianBuffer {
    CasterGaussian gaussians[];
} atlas_gaussian_buffer;

layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer {
    InstanceDataGPU instances[];
} instance_buffer;

layout(set = 0, binding = 2, std430) readonly buffer ChunkMetaBuffer {
    ChunkMetaGPU chunk_meta[];
} chunk_meta_buffer;

layout(set = 0, binding = 3, std430) readonly buffer HlodDrawListBuffer {
    DrawnNodeGPU entries[];
} hlod_draw_list_buffer;

// Per bundle row: 1 = the row casts (row 0 the world; node rows follow cast_shadow). Owned by GsSplatShadowCaster.
layout(set = 0, binding = 4, std430) readonly buffer RowCastBuffer {
    uint row_casts[];
} row_cast_buffer;

// Host mirror: ShadowCasterPushConstant in renderer/gs_shadow_caster.cpp (96 bytes).
layout(push_constant, std430) uniform Params {
    mat4 light_from_frame;  // Godot directional shadow clip transform of a camera-relative position
    vec4 viewport;          // xy = atlas rect size in texels, z = alpha threshold, w = low-pass dilation (texels^2)
    uvec4 counts;           // x = draw records, y = slot stride (max_chunk_splats), z = pancake (1/0), w = opacity multiplier (float bits)
} params;

layout(location = 0) out vec2 v_sigma_uv; // Fragment position in units of sigma along the ellipse's eigen axes.
layout(location = 1) flat out float v_opacity;
// Depth: v_depth_dz is the unclamped mean-depth plane offset g . u (linear, so interpolation is exact); the fragment
// adds it to the flat centre depth and caps it there (gs_shadow_caster::caster_fragment_depth). x = centre depth,
// y = cap (|dz| <= y; 0 = flat quad).
layout(location = 2) out float v_depth_dz;
layout(location = 3) flat out vec2 v_depth_center_cap;

const vec2 GS_CASTER_CORNERS[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

vec3 gs_caster_quat_rotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

vec4 gs_caster_quat_mul(vec4 a, vec4 b) {
    return vec4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz));
}

mat3 gs_caster_quat_to_matrix(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return mat3(
            1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y),
            2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x),
            2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y));
}

void gs_caster_reject() {
    // Outside the clip volume and zero-area: every vertex of the quad lands here.
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_sigma_uv = vec2(0.0);
    v_opacity = 0.0;
    v_depth_dz = 0.0;
    v_depth_center_cap = vec2(0.0);
}

void main() {
    uint stride = params.counts.y;
    if (stride == 0u) {
        gs_caster_reject();
        return;
    }
    uint slot = uint(gl_InstanceIndex);
    uint record = slot / stride;
    uint local_index = slot - record * stride;
    if (record >= params.counts.x || record >= uint(hlod_draw_list_buffer.entries.length())) {
        gs_caster_reject();
        return;
    }
    DrawnNodeGPU drawn = hlod_draw_list_buffer.entries[record];
    if (drawn.chunk_id >= uint(chunk_meta_buffer.chunk_meta.length()) ||
            drawn.instance_id >= uint(instance_buffer.instances.length())) {
        gs_caster_reject();
        return;
    }
    if (drawn.instance_id >= uint(row_cast_buffer.row_casts.length()) || row_cast_buffer.row_casts[drawn.instance_id] == 0u) {
        gs_caster_reject();
        return;
    }
    ChunkMetaGPU chunk = chunk_meta_buffer.chunk_meta[drawn.chunk_id];
    if (local_index >= chunk.splat_count) {
        gs_caster_reject();
        return;
    }
    uint atlas_index = chunk.atlas_base + local_index;
    if (atlas_index < chunk.atlas_base || atlas_index >= uint(atlas_gaussian_buffer.gaussians.length())) {
        gs_caster_reject();
        return;
    }

    // The row's instance opacity (params.x: the node's opacity, _fill_instance_row; 1 for the world) and the renderer-wide
    // opacity multiplier (the world node's opacity, applied by the colour raster to every row) scale the stored opacity
    // exactly as tile_binning.glsl scales it (clamped to [0, 1] there too), before both the threshold test and the
    // support radius: a faded node or world casts what it shows.
    InstanceDataGPU instance = instance_buffer.instances[drawn.instance_id];
    float opacity_multiplier = uintBitsToFloat(params.counts.w);
    float opacity = clamp(atlas_gaussian_buffer.gaussians[atlas_index].opacity * instance.params.x * opacity_multiplier,
            0.0, 1.0);
    float threshold = params.viewport.z;
    // A splat whose peak alpha is below the threshold occludes nowhere.
    if (!(opacity >= threshold) || threshold <= 0.0) {
        gs_caster_reject();
        return;
    }
    vec3 local_position = atlas_gaussian_buffer.gaussians[atlas_index].position;
    vec3 local_scale = atlas_gaussian_buffer.gaussians[atlas_index].scale;
    vec4 local_rotation = atlas_gaussian_buffer.gaussians[atlas_index].rotation;
    float quat_length_sq = dot(local_rotation, local_rotation);
    if (!(quat_length_sq >= 0.81 && quat_length_sq <= 1.21) || any(lessThan(local_scale, vec3(1e-6))) ||
            any(greaterThan(local_scale, vec3(100.0)))) {
        gs_caster_reject();
        return;
    }

    uint instance_flags = instance.ids.y;
    float uniform_scale = abs(instance.translation_scale.w);
    vec3 frame_position = gs_hlod_camera_relative_position(local_position, instance_flags, instance.rotation,
            uniform_scale, drawn.camera_offset.xyz);
    vec3 scale = (instance_flags & GS_INSTANCE_FLAG_SCALE_IDENTITY) != 0u ? local_scale : local_scale * uniform_scale;
    vec4 rotation = (instance_flags & GS_INSTANCE_FLAG_ROTATION_IDENTITY) != 0u
            ? local_rotation
            : gs_caster_quat_mul(instance.rotation, local_rotation);

    vec4 clip = params.light_from_frame * vec4(frame_position, 1.0);
    if (!(abs(clip.w) > 1e-6)) {
        gs_caster_reject();
        return;
    }
    vec3 ndc = clip.xyz / clip.w;

    // Orthographic (directional) projection: the screen Jacobian is the constant linear part of light_from_frame.
    mat3 r = gs_caster_quat_to_matrix(rotation * inversesqrt(quat_length_sq));
    mat3 rs = mat3(r[0] * scale.x, r[1] * scale.y, r[2] * scale.z);
    mat3 cov3 = rs * transpose(rs);
    vec2 half_size = 0.5 * params.viewport.xy;
    // Rows 0 and 1 of the linear part, in texels per unit.
    vec3 jx = vec3(params.light_from_frame[0][0], params.light_from_frame[1][0], params.light_from_frame[2][0]) * half_size.x;
    vec3 jy = vec3(params.light_from_frame[0][1], params.light_from_frame[1][1], params.light_from_frame[2][1]) * half_size.y;
    vec3 cjx = cov3 * jx;
    vec3 cjy = cov3 * jy;
    // Mirrors gs_shadow_caster::footprint_eigenvalues (renderer/hlod_shadow_caster_plan.h): the minor eigenvalue is
    // det / major with the low-pass term added outside the cancelling a * c - b * b; mid - disc loses it in float32
    // for elongated splats and collapses their quads.
    float low_pass = params.viewport.w;
    float a_raw = dot(jx, cjx);
    float b = dot(jx, cjy);
    float c_raw = dot(jy, cjy);
    float a = a_raw + low_pass;
    float c = c_raw + low_pass;
    float mid = 0.5 * (a + c);
    float disc = sqrt(max(mid * mid - (a * c - b * b), 0.0));
    float lambda0 = mid + disc;
    float det = max(a_raw * c_raw - b * b, 0.0) + low_pass * (a_raw + c_raw) + low_pass * low_pass;
    float lambda1 = lambda0 > 0.0 ? max(det / lambda0, max(low_pass, 1e-8)) : 1e-8;
    if (!(lambda0 > 0.0) || isinf(lambda0)) {
        gs_caster_reject();
        return;
    }
    // Major eigenvector from the formulation without cancellation: (lambda0 - c, b) when a dominates, else (b, lambda0 - a).
    // Mirrors gs_shadow_caster::caster_depth_gradient.
    vec2 axis0 = abs(b) > 1e-12 ? normalize(a >= c ? vec2(lambda0 - c, b) : vec2(b, lambda0 - a))
                                : (a >= c ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    vec2 axis1 = vec2(-axis0.y, axis0.x);
    // Exactly the support where alpha >= threshold: opacity * exp(-r^2/2) >= t  <=>  r <= sqrt(2 ln(opacity / t)).
    float sigma_extent = sqrt(max(2.0 * log(opacity / threshold), 0.0));
    vec2 extent0 = axis0 * sqrt(lambda0) * sigma_extent;
    vec2 extent1 = axis1 * sqrt(lambda1) * sigma_extent;
    vec2 extent_ndc = (abs(extent0) + abs(extent1)) / half_size;
    // Cull a quad that cannot touch the cascade rect.
    if (any(greaterThan(abs(ndc.xy), vec2(1.0) + extent_ndc)) || ndc.z < 0.0) {
        gs_caster_reject();
        return;
    }
    float depth = ndc.z;
    bool pancaked = params.counts.z != 0u && depth >= 0.9999;
    if (pancaked) {
        depth = 0.9999; // Pancake: kPancakeDepth in renderer/hlod_shadow_caster_plan.h.
    } else if (depth > 1.0) {
        gs_caster_reject();
        return;
    }

    vec2 corner = GS_CASTER_CORNERS[gl_VertexIndex % 6];
    vec2 offset_texels = extent0 * corner.x + extent1 * corner.y;
    // Depth follows the Gaussian across its footprint: along the light ray through a texel offset u from the centre, the
    // Gaussian's mean depth is z0 + g . u with g = S^-1 c (S = dilated 2D covariance in texels, c = covariance of the texel axes
    // with light-space depth). Linear in u, so per-vertex depth is exact under the orthographic cascade. A constant
    // centre depth instead made every quad a light-facing billboard: on a tilted sheet each splat's billboard stood in
    // front of its down-light neighbours by spacing * slope and shadowed them (#1095 slice 1 review: blotches on a
    // wavy splat receiver). |g . u| is capped at the Gaussian's own depth extent, so an edge-on splat cannot spike. The
    // cap is applied per fragment, not at the quad corners: clamping corners bends the interpolated plane and flattens
    // the slope inside the ellipse too (a corner sits sqrt(2) further out than the ellipse), which brought the
    // self-shadowing back. gl_Position.z stays at the centre so clipping never removes a fragment the cap would keep.
    float plane_dz = 0.0;
    float plane_cap = 0.0;
    if (!pancaked) {
        vec3 jz = vec3(params.light_from_frame[0][2], params.light_from_frame[1][2], params.light_from_frame[2][2]);
        vec3 cjz = cov3 * jz;
        // g = S'^-1 c through the footprint's own eigenbasis (S' = the dilated covariance the quad already uses, so
        // lambda1 >= the low-pass and g is the conditional mean depth slope of the blurred Gaussian). Mirrors
        // gs_shadow_caster::caster_depth_gradient. The adjugate / det form cancelled to det = 0 in float32 for a
        // needle tilted across both axes and dropped the plane (a light-facing billboard again).
        vec2 cxz = vec2(dot(jx, cjz), dot(jy, cjz));
        vec2 g = axis0 * (dot(axis0, cxz) / lambda0) + axis1 * (dot(axis1, cxz) / lambda1);
        plane_cap = sigma_extent * sqrt(max(dot(jz, cjz), 0.0));
        plane_dz = dot(g, offset_texels);
    }
    gl_Position = vec4(ndc.xy + offset_texels / half_size, depth, 1.0);
    v_sigma_uv = corner * sigma_extent;
    v_opacity = opacity;
    v_depth_dz = plane_dz;
    v_depth_center_cap = vec2(depth, plane_cap);
}

#[fragment]

#version 450

layout(location = 0) in vec2 v_sigma_uv;
layout(location = 1) flat in float v_opacity;
layout(location = 2) in float v_depth_dz;
layout(location = 3) flat in vec2 v_depth_center_cap;

layout(push_constant, std430) uniform Params {
    mat4 light_from_frame;
    vec4 viewport;
    uvec4 counts;
} params;

void main() {
    float alpha = v_opacity * exp(-0.5 * dot(v_sigma_uv, v_sigma_uv));
    if (alpha < params.viewport.z) {
        discard;
    }
    // Mirrors gs_shadow_caster::caster_fragment_depth (renderer/hlod_shadow_caster_plan.h). A pancaked caster has
    // cap 0 and its clamped centre depth; a pancake pass keeps every fragment at or in front of kPancakeDepth.
    float depth = v_depth_center_cap.x + clamp(v_depth_dz, -v_depth_center_cap.y, v_depth_center_cap.y);
    if (params.counts.z != 0u) {
        depth = min(depth, 0.9999);
    }
    gl_FragDepth = clamp(depth, 0.0, 1.0);
}
