#ifndef GS_GAME_1_CAPTURE_GLSL
#define GS_GAME_1_CAPTURE_GLSL
// TESTS_ENABLED diagnostic only. The host supplies exact logical keys; absence
// of a write for the current epoch is missing evidence, never zero movement.
#if defined(GS_TESTS_ENABLED) && defined(GS_GAME_1_CAPTURE)
struct GSGame1Sample {
    uvec4 key;
    uvec4 meta;
    vec4 rest;
    vec4 colour;
    vec4 screen;
    vec4 tint;
    vec4 shadow;
    vec4 shadow_anchor;
    vec4 light_0;
    vec4 light_1;
    vec4 light_2;
    vec4 light_3;
    vec4 viewport;
    vec4 local_rest;
};
layout(set = 0, binding = GS_GAME_1_CAPTURE_BINDING, std430) buffer GSGame1Capture {
    uvec4 header; // count, epoch, target-region sort records, target region
    GSGame1Sample samples[];
} gs_game_1;
uint gs_game_1_slot(uint node, uint local_index, uint row) {
    uint count = min(gs_game_1.header.x, uint(gs_game_1.samples.length()));
    for (uint i = 0u; i < count; ++i) {
        if (all(equal(gs_game_1.samples[i].key.xyz, uvec3(node, local_index, row)))) { return i; }
    }
    return 0xffffffffu;
}
#endif
#endif
