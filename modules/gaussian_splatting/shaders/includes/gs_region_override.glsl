#ifndef GS_REGION_OVERRIDE_GLSL
#define GS_REGION_OVERRIDE_GLSL
#include "gs_semantic.glsl"

struct RegionOverrideGPU {
    vec4 tint;
    uvec2 emissive_opacity;
    uint flags;
    uint padding;
};
const uint GS_REGION_HIDE = 1u;
const uint GS_REGION_NO_WIND = 2u;
const uint GS_REGION_HIGHLIGHT = 4u;
const uint GS_REGION_NO_SHADOW = 8u;

vec4 gs_region_emissive_opacity(uvec2 packed_values) {
    vec2 rg = unpackHalf2x16(packed_values.x);
    vec2 ba = unpackHalf2x16(packed_values.y);
    return vec4(rg, ba);
}

vec3 gs_region_tint(vec3 color, vec4 tint, uint flags) {
    vec3 result = color * (1.0 - tint.a) + tint.rgb * tint.a;
    if ((flags & GS_REGION_HIGHLIGHT) != 0u) { result = result * 0.5 + vec3(0.5, 0.5, 0.0); }
    return result;
}

uint gs_region_wind_semantic(uint semantic, uint flags) {
    // B3's shared gs_splat_wind_weight then returns exactly zero, including
    // for legacy payload. Keep the baked region and class bits intact.
    if ((flags & GS_REGION_NO_WIND) != 0u) {
        return (semantic | GS_SEMANTIC_VALID_MASK) & ~GS_SEMANTIC_WIND_MASK;
    }
    return semantic;
}

#ifdef GS_REGION_BINDING
layout(set = 0, binding = GS_REGION_BINDING, std430) readonly buffer RegionOverrideBuffer {
    RegionOverrideGPU entries[];
} region_override_buffer;

RegionOverrideGPU gs_region_resolve(InstanceDataGPU instance, uint semantic, uint atlas_index) {
    RegionOverrideGPU neutral = RegionOverrideGPU(vec4(1.0, 1.0, 1.0, 0.0), uvec2(0u, 0x3c000000u), 0u, 0u);
    uint base = instance.lod.y;
    uint segment_len = instance.placement_pad.x;
    uint count = uint(region_override_buffer.entries.length());
    if (base == 0xffffffffu || segment_len == 0u || base >= count || segment_len > count - base) { return neutral; }
    uint region = (semantic & GS_SEMANTIC_VALID_MASK) != 0u ? semantic & GS_SEMANTIC_REGION_MASK : 0u;
#ifdef GS_TEST_REGION_CONTROLS
#ifndef GS_GAME_1_EXPECTED_REGION_KEY
    if ((instance.ids.y & (1u << 30u)) != 0u) { region = atlas_index; }
#endif
#endif
    return region_override_buffer.entries[base + min(region, segment_len - 1u)];
}
#endif

#endif
