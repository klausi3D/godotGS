#ifndef TILE_PROJECTION_COMMON_GLSL
#define TILE_PROJECTION_COMMON_GLSL

// ProjectedGaussian payload layout. The packed variant trades precision and index range
// for reduced bandwidth; it is enabled via GS_PACKED_STAGE_DATA.
//
// data[1] carries the per-splat linear depth as RAW fp32 (floatBitsToUint). It used to be
// f16-packed, but the f16 far-field quantization (~2.4e-4 x depth range) exceeds the
// composite's depth_epsilon and caused silhouette shimmer when depth-testing splats
// against mesh depth. Opacity + flags moved to the free high 16 bits of the normal-z
// word (data[8] full / data[7] packed) -- gs_unpack_normal reads only the low half of
// that word, so both strides stay unchanged (36 B / 32 B).
#ifdef GS_PACKED_STAGE_DATA
struct ProjectedGaussian {
    uint data[8];
};
#else
struct ProjectedGaussian {
    uint data[9];
};
#endif

// Pack screen-space position into two half-floats.
uint gs_pack_screen_xy(vec2 screen_pos) {
    return packHalf2x16(screen_pos);
}

// Pack the Z normal component (low 16 bits, half) plus opacity (unorm8) and 8 bits of
// flags (high 16 bits) into one 32-bit word. Replaces the separate gs_pack_normal_zw +
// depth-word opacity: the high half of the normal-z word was unused, and reclaiming it
// frees data[1] to carry raw fp32 depth (see header comment).
uint gs_pack_normal_z_opacity_flags(vec3 normal, float opacity, uint flags) {
    uint z_bits = packHalf2x16(vec2(normal.z, 0.0)) & 0xFFFFu;
    uint opacity_bits = uint(clamp(opacity, 0.0, 1.0) * 255.0 + 0.5) & 0xFFu;
    return z_bits | (opacity_bits << 16u) | ((flags & 0xFFu) << 24u);
}

// Pack linear RGB into the tile color payload format.
uint gs_pack_color_r11g11b10(vec3 color) {
    // Pack using RGB11F/10F style encoding stored in 16-bit halves.
    color = clamp(color, vec3(0.0), vec3(65504.0));

    uint rg_packed = packHalf2x16(color.rg);
    uint b_packed = packHalf2x16(vec2(color.b, 0.0));

    uint r_f16 = rg_packed & 0xFFFFu;
    uint g_f16 = (rg_packed >> 16u) & 0xFFFFu;
    uint b_f16 = b_packed & 0xFFFFu;

    uint r_exp = (r_f16 >> 10u) & 0x1Fu;
    uint r_mant = (r_f16 & 0x3FFu) >> 4u;
    uint g_exp = (g_f16 >> 10u) & 0x1Fu;
    uint g_mant = (g_f16 & 0x3FFu) >> 4u;
    uint b_exp = (b_f16 >> 10u) & 0x1Fu;
    uint b_mant = (b_f16 & 0x3FFu) >> 5u;

    uint r11 = (r_exp << 6u) | r_mant;
    uint g11 = (g_exp << 6u) | g_mant;
    uint b10 = (b_exp << 5u) | b_mant;

    return r11 | (g11 << 11u) | (b10 << 22u);
}

// Pack the X/Y normal components into one 32-bit word.
uint gs_pack_normal_xy(vec3 normal) {
    return packHalf2x16(normal.xy);
}

// Legacy function kept for API compatibility.
//
// IMPORTANT: this packs `global_idx` into the high 16 bits and the rasterizer
// (tile_raster_common.glsl, `if (stored_global_idx != sorted_idx) continue;`)
// will silently reject any visible splat whose actual global_idx exceeds
// UINT16_MAX = 65535. Callers must ensure `enable_packed_stage_data` is only
// used when the packed-stage payload count is <= 65535. Runtime enforcement is
// the TileRenderer disable gate (renderer/tile_renderer.cpp ~ line 415), which
// has the scene payload count; PipelineFeatureSet only carries the shared
// PACKED_STAGE_MAX_TOTAL_SPLATS = 65535 capability constant. The
// GS_PACKED_STAGE_DATA define is emitted only after the TileRenderer gate.
// Do not relax these without redesigning the packed payload to carry a full
// 32-bit global_idx.
uint gs_pack_conic_y_and_index(float conic_y, uint global_idx) {
    uint conic_y_bits = packHalf2x16(vec2(conic_y, 0.0)) & 0xFFFFu;
    uint idx_bits = (global_idx & 0xFFFFu);
    return conic_y_bits | (idx_bits << 16u);
}

// Unpack the packed screen-space position.
vec2 gs_unpack_screen_xy(uint packed) {
    return unpackHalf2x16(packed);
}

// Unpack opacity and flags from the high 16 bits of the normal-z word.
void gs_unpack_opacity_flags(uint packed, out float opacity, out uint flags) {
    opacity = float((packed >> 16u) & 0xFFu) / 255.0;
    flags = (packed >> 24u) & 0xFFu;
}

// Unpack the tile color payload back into linear RGB.
vec3 gs_unpack_color_r11g11b10(uint packed) {
    uint r11 = packed & 0x7FFu;
    uint g11 = (packed >> 11u) & 0x7FFu;
    uint b10 = (packed >> 22u) & 0x3FFu;

    uint r_exp = (r11 >> 6u) & 0x1Fu;
    uint r_mant = (r11 & 0x3Fu) << 4u;
    uint g_exp = (g11 >> 6u) & 0x1Fu;
    uint g_mant = (g11 & 0x3Fu) << 4u;
    uint b_exp = (b10 >> 5u) & 0x1Fu;
    uint b_mant = (b10 & 0x1Fu) << 5u;

    uint r_f16 = (r_exp << 10u) | r_mant;
    uint g_f16 = (g_exp << 10u) | g_mant;
    uint b_f16 = (b_exp << 10u) | b_mant;

    vec2 rg = unpackHalf2x16(r_f16 | (g_f16 << 16u));
    float b = unpackHalf2x16(b_f16).x;

    return vec3(rg, b);
}

// Unpack the normal payload back into a 3D normal vector.
vec3 gs_unpack_normal(uint packed_xy, uint packed_zw) {
    vec2 xy = unpackHalf2x16(packed_xy);
    vec2 zw = unpackHalf2x16(packed_zw);
    return vec3(xy, zw.x);
}

// Legacy function kept for API compatibility
void gs_unpack_conic_y_and_index(uint packed, out float conic_y, out uint global_idx) {
    conic_y = unpackHalf2x16(packed & 0xFFFFu).x;
    global_idx = (packed >> 16u) & 0xFFFFu;
}

// Unpack a projected Gaussian payload into raster-friendly fields.
void gs_unpack_projected_gaussian(in ProjectedGaussian pg,
        out vec2 screen_pos, out float depth, out float opacity,
        out vec3 color, out vec3 normal, out vec3 conic, out uint global_idx) {
    screen_pos = gs_unpack_screen_xy(pg.data[0]);

    depth = uintBitsToFloat(pg.data[1]);

    color = gs_unpack_color_r11g11b10(pg.data[2]);

    conic.x = uintBitsToFloat(pg.data[3]);
    conic.z = uintBitsToFloat(pg.data[4]);

    uint flags;
#ifdef GS_PACKED_STAGE_DATA
    gs_unpack_conic_y_and_index(pg.data[5], conic.y, global_idx);
    // gs_unpack_normal reads only the low half of data[7]; opacity/flags live in its high half.
    normal = gs_unpack_normal(pg.data[6], pg.data[7]);
    gs_unpack_opacity_flags(pg.data[7], opacity, flags);
#else
    conic.y = uintBitsToFloat(pg.data[5]);
    global_idx = pg.data[6];
    normal = gs_unpack_normal(pg.data[7], pg.data[8]);
    gs_unpack_opacity_flags(pg.data[8], opacity, flags);
#endif
}

// Pack raster-ready projected Gaussian fields into the payload layout.
void gs_pack_projected_gaussian(out ProjectedGaussian pg,
        vec2 screen_pos, float depth, float opacity,
        vec3 color, vec3 normal, vec3 conic, uint global_idx) {
    pg.data[0] = gs_pack_screen_xy(screen_pos);
    pg.data[1] = floatBitsToUint(depth);
    pg.data[2] = gs_pack_color_r11g11b10(color);
    pg.data[3] = floatBitsToUint(conic.x);
    pg.data[4] = floatBitsToUint(conic.z);
#ifdef GS_PACKED_STAGE_DATA
    pg.data[5] = gs_pack_conic_y_and_index(conic.y, global_idx);
    pg.data[6] = gs_pack_normal_xy(normal);
    pg.data[7] = gs_pack_normal_z_opacity_flags(normal, opacity, 0u);
#else
    pg.data[5] = floatBitsToUint(conic.y);
    pg.data[6] = global_idx;
    pg.data[7] = gs_pack_normal_xy(normal);
    pg.data[8] = gs_pack_normal_z_opacity_flags(normal, opacity, 0u);
#endif
}

// Compute the linear index for a tile/slot pair in the projection buffer.
uint tile_projection_index(uint tile_index, uint slot_index) {
    return tile_index * uint(SPLATS_PER_TILE) + slot_index;
}

#endif // TILE_PROJECTION_COMMON_GLSL
