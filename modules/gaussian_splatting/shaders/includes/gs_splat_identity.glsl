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

// History tag only: node is the baked node / asset-local chunk; placement is the full ObjectID.
uint gs_splat_identity_hash(uint node, uint local, uvec2 placement) {
    uint h = gs_identity_hash_u32(node ^ 0x9e3779b9u);
    h = gs_identity_hash_u32(h ^ local);
    h = gs_identity_hash_u32(h ^ placement.x);
    return gs_identity_hash_u32(h ^ placement.y);
}

// Worlds fold the baked node ID modulo 2^(31-L), preserving the zero world
// prefix and every local bit. Below that node-count limit this is bit-identical.
// The full node ID still tags history. Instances retain the host's 1+R+C+L
// admission check and never fold their asset-local chunk ID.
uint gs_splat_composite_key(uint node, uint local, uint prefix, uint local_bits,
        uint atlas_index, bool canonical_single) {
#ifndef GS_HLOD_RELATIVE_PAYLOAD
    // #792: preserve the canonical singleton's exact global-index ordering.
    if (canonical_single) { return atlas_index; }
#endif
    if (prefix == 0u) { node &= 0x7fffffffu >> local_bits; }
    return prefix | (node << local_bits) | local;
}

uint gs_identity_tie_break(uint stable_key, uint atlas_index) {
#ifdef GS_ADDRESS_IDENTITY_ACTIVE
    return atlas_index;
#else
    return stable_key;
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
