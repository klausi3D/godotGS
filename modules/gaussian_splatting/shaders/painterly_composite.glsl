#[vertex]

#version 450

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2(3.0, -1.0),
    vec2(-1.0, 3.0)
);

// Vertex entry point for the fullscreen composite triangle.
void main() {
    vec2 pos = positions[gl_VertexIndex];
    gl_Position = vec4(pos, 0.0, 1.0);
}

#[fragment]

#version 450

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D painterly_color;
layout(set = 0, binding = 1) uniform sampler2D painterly_depth;
layout(set = 0, binding = 2) uniform sampler2D scene_depth;

#include "includes/gs_srgb.glsl"
#include "includes/gs_scene_depth_guard.glsl"

// Mirror of GaussianRenderPipeline::PainterlyCompositePushConstant
// (renderer/render_types/render_pipeline_io_types.h). `pad` is NOT optional:
// SPIRV-Reflect reports a push-constant block's size rounded UP to a 16-byte
// multiple (spirv_reflect.c:2785-2803, SPIRV_DATA_ALIGNMENT = 16), Godot adopts
// that as the pipeline's required size (rendering_device_commons.cpp:1292) and
// then demands the host supply EXACTLY that many bytes
// (rendering_device.cpp:4745). The payload scalars occupy 40 bytes, so the
// reflected requirement is 48; without the pad the host would push 40 and every
// draw would be rejected (#986 blocker B). Keep this block a multiple of 16.
//
// depth_linearize_mul / depth_linearize_add / depth_is_orthogonal come from the
// ONE host derivation (interfaces/gs_scene_depth_linearize.h) the compute blit
// also uses. This pass previously carried raw projection columns and linearized
// scene depth its own way, which never matched the depth buffer and discarded
// every splat fragment once the composite became reachable (#986).
layout(push_constant, std430) uniform CompositePush {
    vec2 inv_viewport_size;
    float depth_epsilon;
    float blend_strength;
    float z_near;
    float z_far;
    float depth_linearize_mul;
    float depth_linearize_add;
    int depth_is_orthogonal;
    int depth_test_enabled;
    float pad[2];
} params;

// Fragment entry point for the fullscreen composite pass.
void main() {
    vec2 sample_uv = clamp(gl_FragCoord.xy * params.inv_viewport_size, vec2(0.0), vec2(1.0));

    vec4 painterly_sample = texture(painterly_color, sample_uv);
    if (painterly_sample.a <= 0.0001) {
        discard;
    }

    float painterly_depth_value = texture(painterly_depth, sample_uv).r;
    if (painterly_depth_value <= 0.0) {
        discard;
    }

    // Honour composite/depth_test, the same setting the compute composite reads.
    // Both source and scene depth are validated by the host before this pass is
    // submitted, so the strict scene-depth contract is satisfied by construction.
    if (params.depth_test_enabled != 0) {
        float scene_depth_value = texture(scene_depth, sample_uv).r;
        if (gs_scene_depth_occludes(scene_depth_value, painterly_depth_value, params.depth_is_orthogonal,
                params.z_near, params.z_far, params.depth_linearize_mul, params.depth_linearize_add,
                params.depth_epsilon)) {
            discard;
        }
    }

    float alpha = clamp(painterly_sample.a * params.blend_strength, 0.0, 1.0);
    if (alpha <= 0.0) {
        discard;
    }

    // Source-encoding contract (#930, interfaces/output_compositor_interfaces.h):
    // the painterly raster output is PREMULTIPLIED, display-referred, sRGB-encoded
    // LDR, while this pass always draws into render_buffers->get_internal_texture()
    // -- the LINEAR pre-tonemap scene buffer. sRGB decode does not commute with
    // alpha premultiplication, so unpremultiply, decode, re-premultiply, exactly as
    // the compute composite does (viewport_blit.glsl, params.source_decode_srgb).
    // The `painterly_sample.a <= 0.0001` early-out above guarantees the divisor.
    vec3 straight_srgb = painterly_sample.rgb / painterly_sample.a;
    out_color = vec4(srgb_to_linear_exact(straight_srgb) * alpha, alpha);
}
