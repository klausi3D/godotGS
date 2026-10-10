#ifndef TILE_PROJECTION_COMMON_GLSL
#define TILE_PROJECTION_COMMON_GLSL

// C4b (G4) channel bits of the resident OverflowStats.overflow_drop_signal word (binding 3).
// Every writer uses atomicOr so channels accumulate instead of masking each other. Host
// mirror: OVERFLOW_DROP_SIGNAL_* in renderer/tile_render_types.h (keep the values equal).
#define GS_OVERFLOW_DROP_BINNING 1u          // tile-binning EMIT dropped an overlap record
#define GS_OVERFLOW_DROP_RASTER_TILE_CAP 2u  // a rasterizer truncated a tile at GS_MAX_RASTER_SPLATS_PER_TILE (#1137)

// ProjectedGaussian payload layout. The packed variant trades precision and index range
// for reduced bandwidth; it is enabled via GS_PACKED_STAGE_DATA.
//
// data[1] carries the per-splat linear depth as RAW fp32 (floatBitsToUint). It used to be
// f16-packed, but the f16 far-field quantization (~2.4e-4 x depth range) exceeds the
// composite's depth_epsilon and caused silhouette shimmer when depth-testing splats
// against mesh depth. Opacity + flags moved to the free high 16 bits of the normal-z
// word (data[8] full / data[7] packed) -- gs_unpack_normal reads only the low half of
// that word.
//
// The screen centre is RAW fp32 too (#1153): x in data[0], y in the last word
// (GS_PG_SCREEN_Y_WORD). It used to be packHalf2x16 in pixels, whose spacing is 1 px for
// x in [1024, 2048) and 2 px above, so the right half of a 1080p frame rasterized every
// centre snapped to an integer pixel and sub-pixel TAA jitter could not move it. A
// viewport-normalised unorm16 would have kept the stride but clamps off-screen centres,
// which large splats overlapping the screen edge legitimately have. Strides: 40 B full,
// 36 B packed (host mirror: TileProjectionLayout in renderer/tile_render_types.h).
#ifdef GS_PACKED_STAGE_DATA
#define GS_PG_SCREEN_Y_WORD 8
struct ProjectedGaussian {
    uint data[9];
};
#else
#define GS_PG_SCREEN_Y_WORD 9
struct ProjectedGaussian {
    uint data[10];
};
#endif

// Pack the Z normal component (low 16 bits, half) plus opacity (unorm8) and 8 bits of
// flags (high 16 bits) into one 32-bit word. Replaces the separate gs_pack_normal_zw +
// depth-word opacity: the high half of the normal-z word was unused, and reclaiming it
// frees data[1] to carry raw fp32 depth (see header comment).
uint gs_pack_normal_z_opacity_flags(vec3 normal, float opacity, uint flags) {
    uint z_bits = packHalf2x16(vec2(normal.z, 0.0)) & 0xFFFFu;
    uint opacity_bits = uint(clamp(opacity, 0.0, 1.0) * 255.0 + 0.5) & 0xFFu;
    return z_bits | (opacity_bits << 16u) | ((flags & 0xFFu) << 24u);
}

// Round an fp16 bit pattern to nearest (ties to even) at `drop_bits` below its mantissa
// LSB, so the caller's right shift keeps the nearest value instead of truncating
// (#1168: truncation biased every payload colour dark; over [0.5, 1) by about -0.9 / -1.9
// 8-bit LSB on average on R,G / B, and by up to one full quantum). A carry out of the mantissa bumps the exponent, which is the correct
// rounding; the result saturates at the max finite half (0x7BFF) so 65504 cannot round
// into the infinity exponent.
uint gs_round_f16_for_shift(uint h, uint drop_bits) {
    uint half_step = ((1u << (drop_bits - 1u)) - 1u) + ((h >> drop_bits) & 1u);
    return min(h + half_step, 0x7BFFu);
}

// True when every channel is a finite number. A NaN/Inf colour must not reach
// gs_pack_color_r11g11b10: clamp() on NaN is undefined and the R11G11B10 pack keeps the
// fp16 NaN/Inf exponent, which the raster then accumulates over the whole footprint.
bool gs_color_is_finite(vec3 color) {
    return !(any(isnan(color)) || any(isinf(color)));
}

// Pack linear RGB into the tile color payload format. Callers must pass a finite colour
// (see gs_color_is_finite).
uint gs_pack_color_r11g11b10(vec3 color) {
    // Pack using RGB11F/10F style encoding stored in 16-bit halves.
    color = clamp(color, vec3(0.0), vec3(65504.0));

    uint rg_packed = packHalf2x16(color.rg);
    uint b_packed = packHalf2x16(vec2(color.b, 0.0));

    uint r_f16 = gs_round_f16_for_shift(rg_packed & 0xFFFFu, 4u);
    uint g_f16 = gs_round_f16_for_shift((rg_packed >> 16u) & 0xFFFFu, 4u);
    uint b_f16 = gs_round_f16_for_shift(b_packed & 0xFFFFu, 5u);

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
    screen_pos = vec2(uintBitsToFloat(pg.data[0]), uintBitsToFloat(pg.data[GS_PG_SCREEN_Y_WORD]));

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
    pg.data[0] = floatBitsToUint(screen_pos.x);
    pg.data[GS_PG_SCREEN_Y_WORD] = floatBitsToUint(screen_pos.y);
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
