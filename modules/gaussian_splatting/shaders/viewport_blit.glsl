#[compute]

#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D u_source_texture;
layout(set = 0, binding = 2) uniform sampler2D u_source_depth;
layout(set = 0, binding = 3) uniform sampler2D u_destination_depth;

// Sampled copy of the destination, captured to a separate texture before this
// dispatch by OutputCompositor::_copy_final_output_compute. Reading the live
// destination via imageLoad inside the same compute dispatch that writes it
// reintroduces the cross-invocation R/W hazard responsible for incident #256
// (workgroup-grid black blocks on AMD/RDNA drivers). The destination image
// below is now writeonly; all reads of the prior destination go through this
// scratch sampler.
layout(set = 0, binding = 4) uniform sampler2D u_destination_scratch;

#ifndef VIEWPORT_BLIT_FORMAT
#define VIEWPORT_BLIT_FORMAT 0
#endif

#if VIEWPORT_BLIT_FORMAT == 0
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D u_destination_image;
#elif VIEWPORT_BLIT_FORMAT == 1
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D u_destination_image;
#elif VIEWPORT_BLIT_FORMAT == 2
layout(set = 0, binding = 1, rgba32f) uniform writeonly image2D u_destination_image;
#else
#error "Unsupported VIEWPORT_BLIT_FORMAT value"
#endif

layout(push_constant, std430) uniform BlitParams {
    ivec2 copy_size;
    ivec2 source_size;
    ivec2 destination_size;
    ivec2 destination_offset;
    int composite_with_destination;
    int destination_is_srgb;
    int source_is_premultiplied;
    int depth_test_enabled;
    int depth_is_orthogonal;
    float z_near;
    float z_far;
    float depth_epsilon;
    float depth_linearize_mul;
    float depth_linearize_add;
    // Occupies the former pad0 slot (same 4-byte size/alignment; all other
    // offsets unchanged). Mirrors `int32_t source_decode_srgb` in the host
    // ViewportBlitPushConstant (output_compositor.cpp). Non-zero when the
    // destination is the LINEAR pre-tonemap scene buffer and the sRGB-encoded
    // LDR splat source must be decoded to linear before blending (GPU-001
    // Option B source-encoding contract).
    int source_decode_srgb;
    float pad1;
} params;

// Fast sRGB approximations using polynomial/sqrt instead of pow()
// These avoid expensive pow() calls while maintaining good accuracy
// Max error ~0.4% which is imperceptible

vec3 srgb_to_linear(vec3 color) {
    // Fast polynomial approximation: srgb^2.2 ≈ 0.0125*s + 0.682*s² + 0.305*s³
    vec3 srgb = clamp(color, vec3(0.0), vec3(1.0));
    vec3 srgb2 = srgb * srgb;
    vec3 srgb3 = srgb2 * srgb;
    return 0.012522878 * srgb + 0.682171111 * srgb2 + 0.305306011 * srgb3;
}

// Convert linear color to sRGB for final viewport output.
vec3 linear_to_srgb(vec3 color) {
    // Fast approximation using nested sqrt: linear^(1/2.2) ≈ polynomial of sqrt(sqrt(sqrt(x)))
    // 3 sqrt ops + 4 mul + 3 add is faster than pow() transcendental
    vec3 linear = max(color, vec3(0.0));
    vec3 S1 = sqrt(linear);
    vec3 S2 = sqrt(S1);
    vec3 S3 = sqrt(S2);
    return clamp(0.662002687 * S1 + 0.684122060 * S2 - 0.323583601 * S3 - 0.0225411470 * linear, vec3(0.0), vec3(1.0));
}

// Convert raw scene depth to linear view-space depth.
float linearize_scene_depth(float raw_depth) {
    if (params.depth_is_orthogonal != 0) {
        float ndc = raw_depth * 2.0 - 1.0;
        return -(ndc * (params.z_far - params.z_near) - (params.z_far + params.z_near)) / 2.0;
    }
    return params.depth_linearize_mul / (params.depth_linearize_add - raw_depth);
}

// Clamp invalid depth values to a stable far-plane fallback.
float sanitize_view_depth(float depth_value) {
    if (isnan(depth_value) || isinf(depth_value)) {
        return -1.0;
    }
    return abs(depth_value);
}

// Detect background depth samples near the far plane.
bool is_scene_background_depth(float raw_scene_depth, float scene_view_depth) {
    float scene_depth_from_zero = sanitize_view_depth(linearize_scene_depth(0.0));
    float scene_depth_from_one = sanitize_view_depth(linearize_scene_depth(1.0));
    if (scene_depth_from_zero < 0.0 || scene_depth_from_one < 0.0) {
        return false;
    }

    float clear_raw_depth = scene_depth_from_zero >= scene_depth_from_one ? 0.0 : 1.0;
    float far_view_depth = max(scene_depth_from_zero, scene_depth_from_one);
    float far_tolerance = max(params.depth_epsilon * 2.0, 1e-3);

    bool raw_matches_clear = abs(raw_scene_depth - clear_raw_depth) <= 1e-6;
    bool view_matches_far = abs(scene_view_depth - far_view_depth) <= far_tolerance;
    return raw_matches_clear || view_matches_far;
}

// Blit the rendered viewport into the final output target.
void main() {
    ivec2 local_coord = ivec2(gl_GlobalInvocationID.xy);
    if (local_coord.x >= params.copy_size.x || local_coord.y >= params.copy_size.y) {
        return;
    }

    ivec2 destination_coord = params.destination_offset + local_coord;
    if (destination_coord.x < 0 || destination_coord.y < 0 || destination_coord.x >= params.destination_size.x || destination_coord.y >= params.destination_size.y) {
        return;
    }

    vec2 source_uv = (vec2(local_coord) + vec2(0.5)) / vec2(max(params.source_size, ivec2(1)));

    if (params.depth_test_enabled != 0) {
        ivec2 source_depth_size = textureSize(u_source_depth, 0);
        ivec2 destination_depth_size = textureSize(u_destination_depth, 0);
        bool destination_depth_in_bounds = destination_coord.x < destination_depth_size.x &&
                destination_coord.y < destination_depth_size.y;

        // Address the GS depth through the SAME normalized source_uv as the color so the
        // two stay aligned when the depth texture's size differs from the color source
        // (possible on the painterly path). Nearest fetch with a scaled+clamped coord --
        // NOT a linear textureLod: filtered depth across silhouettes fabricates mid-edge
        // depths, which is the artifact the depth test exists to prevent. Bit-identical
        // to the old local_coord fetch when the sizes match (the common case).
        ivec2 gs_depth_coord = clamp(ivec2(source_uv * vec2(source_depth_size)),
                ivec2(0), source_depth_size - 1);
        float gs_depth = texelFetch(u_source_depth, gs_depth_coord, 0).r;
        float scene_depth = destination_depth_in_bounds ? texelFetch(u_destination_depth, destination_coord, 0).r : 1.0;
        bool depths_in_range = (gs_depth >= 0.0 && gs_depth <= 1.0 &&
                scene_depth >= 0.0 && scene_depth <= 1.0);

        if (depths_in_range && gs_depth < 0.999999) {
            float scene_view_depth = sanitize_view_depth(linearize_scene_depth(scene_depth));
            float gs_view_depth = sanitize_view_depth(mix(params.z_near, params.z_far, clamp(gs_depth, 0.0, 1.0)));
            bool scene_is_background = is_scene_background_depth(scene_depth, scene_view_depth);

            if (!scene_is_background && scene_view_depth >= 0.0 && gs_view_depth >= 0.0 &&
                    scene_view_depth <= gs_view_depth - params.depth_epsilon) {
                return;
            }
        }
    }
    vec4 source_color = texture(u_source_texture, source_uv);
    vec4 destination_color = vec4(0.0);
    if (params.composite_with_destination != 0) {
        destination_color = texelFetch(u_destination_scratch, destination_coord, 0);
    }

    vec4 source_linear = source_color;
    vec4 destination_linear = destination_color;

    if (params.source_decode_srgb != 0) {
        // Source-encoding contract (GPU-001 Option B): the splat raster output is
        // display-referred, sRGB-encoded LDR. To inject into a linear destination
        // the color must be decoded on STRAIGHT (un-premultiplied) values —
        // sRGB decode does not commute with alpha premultiplication — so
        // unpremultiply, decode, re-premultiply. Fully transparent texels carry
        // no color and are handled by the early-outs below.
        if (params.source_is_premultiplied != 0) {
            if (source_linear.a > 0.0) {
                vec3 straight = source_linear.rgb / source_linear.a;
                source_linear.rgb = srgb_to_linear(straight) * source_linear.a;
            }
        } else {
            source_linear.rgb = srgb_to_linear(source_linear.rgb);
        }
    }

    if (params.composite_with_destination != 0 && source_linear.a <= 0.0) {
        // Destination already contains the original pixel; with the writeonly
        // image binding we leave it untouched by returning without imageStore.
        return;
    }

    if (params.destination_is_srgb != 0 && params.composite_with_destination != 0) {
        destination_linear.rgb = srgb_to_linear(destination_linear.rgb);
    }

    vec4 result_color = source_linear;

    if (params.composite_with_destination != 0) {
        vec3 src_rgb = source_linear.rgb;
        // Treat destination as fully opaque (alpha=1). Godot's post-tonemap
        // framebuffer does not carry meaningful alpha — sky, meshes, and
        // background are all opaque content with undefined alpha channel.
        vec3 dst_rgb = destination_linear.rgb;

        if (params.source_is_premultiplied == 0) {
            src_rgb *= source_linear.a;
        }

        // Premultiplied source over opaque destination.
        vec3 out_rgb = src_rgb + dst_rgb * (1.0 - source_linear.a);

        result_color.rgb = out_rgb;
        result_color.a = 1.0;
    }

    if (params.destination_is_srgb != 0) {
        result_color.rgb = linear_to_srgb(result_color.rgb);
    }

    imageStore(u_destination_image, destination_coord, result_color);
}
