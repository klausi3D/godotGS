#ifndef GS_SPLAT_IDENTITY_GLSL
#define GS_SPLAT_IDENTITY_GLSL

// The forcing define alone cannot activate a production shader.
#ifdef GS_TESTS_ENABLED
#ifdef GS_TEST_ADDRESS_IDENTITY
#define GS_ADDRESS_IDENTITY_ACTIVE
#endif
#endif
#ifndef GS_HLOD_RELATIVE_PAYLOAD
#define GS_ADDRESS_HISTORY
#endif
#ifdef GS_ADDRESS_IDENTITY_ACTIVE
#define GS_ADDRESS_HISTORY
#endif

uint gs_identity_hash_u32(uint v) {
    v ^= v >> 16u;
    v *= 0x7feb352du;
    v ^= v >> 15u;
    v *= 0x846ca68bu;
    return v ^ (v >> 16u);
}

// chunk_id is the stable node index; placement is the full ObjectID, never a row.
uint gs_splat_identity_hash(uint node, uint local, uvec2 placement) {
    uint h = gs_identity_hash_u32(node ^ 0x9e3779b9u);
    h = gs_identity_hash_u32(h ^ local);
    h = gs_identity_hash_u32(h ^ placement.x);
    return gs_identity_hash_u32(h ^ placement.y);
}

uint gs_identity_tie_break(uint identity_hash, uint atlas_index) {
#ifdef GS_ADDRESS_IDENTITY_ACTIVE
    return atlas_index;
#else
    return identity_hash;
#endif
}

// Reverse entries are absolute forward-table slots. The selected node's slot
// interval proves ownership and recovers the local page without a forward scan.
bool gs_hlod_local_splat_index(uint atlas_index, uint reverse_slot, uint page_table_ref,
        uint atlas_base, uint splat_count, out uint local_index) {
    local_index = 0u;
    if (page_table_ref == 0u) {
        if (atlas_index < atlas_base) { return false; }
        local_index = atlas_index - atlas_base;
        return local_index < splat_count;
    }
    uint base = page_table_ref - 1u;
    if (reverse_slot == 0xffffffffu || reverse_slot < base) { return false; }
    uint local_page = reverse_slot - base;
    uint page_count = splat_count >> GS_HLOD_PAGE_SHIFT;
    if ((splat_count & GS_HLOD_PAGE_MASK) != 0u) { page_count += 1u; }
    if (local_page >= page_count) { return false; }
    local_index = (local_page << GS_HLOD_PAGE_SHIFT) | (atlas_index & GS_HLOD_PAGE_MASK);
    return local_index < splat_count;
}

#endif
