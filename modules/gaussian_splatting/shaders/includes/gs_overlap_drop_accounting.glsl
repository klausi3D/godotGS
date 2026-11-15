// Overlap-record drop accounting shared by the tile rasterizers and the host (#1457, #1461).
// Plain uint arithmetic only (no GLSL built-ins), so renderer/overlap_drop_accounting.h compiles
// this same text in C++ and the host tests exercise the arithmetic the shaders run.
#ifndef GS_OVERLAP_DROP_ACCOUNTING_GLSL
#define GS_OVERLAP_DROP_ACCOUNTING_GLSL
#ifndef GS_DROP_INLINE
#define GS_DROP_INLINE
#endif

// Records of one tile range [range_start, range_start + range_count) that the rasterizer adds to
// overflow_splats_clamped for its availability clamp: the records tile binning's EMIT pass WROTE
// (index below emit_limit, indirect_dispatch.element_count) that the rasterizer cannot READ (index
// at or past readable_limit, that count clamped to the sorted-value capacity).
//
// Records at or past emit_limit are not counted here. EMIT dropped each of them and counted it
// once (tile_binning.glsl); counting the cut tile's tail again was the #1457 double count. With
// readable_limit == emit_limit, the normal case, this is 0 for every tile.
GS_DROP_INLINE uint gs_raster_unreadable_written_records(uint range_start, uint range_count, uint emit_limit,
        uint readable_limit) {
    uint range_end = range_start + range_count;
    if (range_end < range_start) {
        range_end = 0xffffffffu; // a corrupt range must not wrap
    }
    // Both limits clamped into the tile's range; readable_limit never exceeds emit_limit.
    uint written_end = emit_limit < range_start ? range_start : (emit_limit > range_end ? range_end : emit_limit);
    uint readable_end = readable_limit < range_start ? range_start : (readable_limit > range_end ? range_end : readable_limit);
    return written_end > readable_end ? written_end - readable_end : 0u;
}

// Per-tile record maximum (#1461). A frame's rasterizer raises two words with atomicMax: the
// exact maximum record count of any tile, and this key, which orders tiles by record count
// first and tile index second. The key identifies the tile only while the count fits
// GS_MAX_TILE_KEY_RECORDS_MAX and the index fits GS_MAX_TILE_KEY_TILE_MASK; the host checks the
// key against the exact maximum (gs_max_tile_key_identifies) and reports the tile as not
// measured otherwise, never as tile 0.
const uint GS_MAX_TILE_KEY_TILE_BITS = 15u;
const uint GS_MAX_TILE_KEY_TILE_MASK = 0x7fffu;
const uint GS_MAX_TILE_KEY_RECORDS_MAX = 0x1ffffu;

GS_DROP_INLINE uint gs_pack_max_tile_key(uint records, uint tile_idx) {
    if (tile_idx > GS_MAX_TILE_KEY_TILE_MASK) {
        return 0u; // not encodable: never wins against an encodable tile with records
    }
    uint clamped_records = records > GS_MAX_TILE_KEY_RECORDS_MAX ? GS_MAX_TILE_KEY_RECORDS_MAX : records;
    return (clamped_records << GS_MAX_TILE_KEY_TILE_BITS) | tile_idx;
}

GS_DROP_INLINE uint gs_max_tile_key_records(uint key) {
    return key >> GS_MAX_TILE_KEY_TILE_BITS;
}

GS_DROP_INLINE uint gs_max_tile_key_tile(uint key) {
    return key & GS_MAX_TILE_KEY_TILE_MASK;
}

// True when the key's tile is a tile that holds exactly max_records records.
GS_DROP_INLINE bool gs_max_tile_key_identifies(uint key, uint max_records) {
    return max_records > 0u && max_records <= GS_MAX_TILE_KEY_RECORDS_MAX && gs_max_tile_key_records(key) == max_records;
}

#endif
