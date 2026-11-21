#ifndef GS_REGION_OVERRIDE_GLSL
#define GS_REGION_OVERRIDE_GLSL

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

vec4 gs_region_emissive_opacity(uvec2 packed) {
    return vec4(0.0, 0.0, 0.0, 1.0); // Inert tests-first seam.
}

vec3 gs_region_tint(vec3 color, vec4 tint, uint flags) {
    return color; // Inert tests-first seam.
}

uint gs_region_wind_semantic(uint semantic, uint flags) { return semantic; }

#endif
