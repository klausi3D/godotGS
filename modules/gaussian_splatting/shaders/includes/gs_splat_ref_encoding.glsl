// Visible reference word: low29 bits physical atlas index, high3 bits SH degree.
#ifndef GS_SPLAT_REF_ENCODING_GLSL
#define GS_SPLAT_REF_ENCODING_GLSL
#ifndef GS_REF_INLINE
#define GS_REF_INLINE
#endif
const uint GS_SPLAT_REF_ATLAS_INDEX_MASK = 0x1fffffffu;
GS_REF_INLINE bool gs_splat_ref_can_encode(uint atlas_index) {
    return atlas_index <= GS_SPLAT_REF_ATLAS_INDEX_MASK;
}
GS_REF_INLINE uint gs_pack_splat_ref_atlas_index(uint atlas_index, uint sh_limit) {
    if (!gs_splat_ref_can_encode(atlas_index)) return 0xffffffffu;
    return atlas_index | ((sh_limit > 4u ? 4u : sh_limit) << 29u);
}
GS_REF_INLINE uint gs_splat_ref_atlas_index(uint encoded) {
    return encoded & GS_SPLAT_REF_ATLAS_INDEX_MASK;
}
GS_REF_INLINE uint gs_splat_ref_sh_limit(uint encoded) {
    uint degree = encoded >> 29u;
    return degree > 4u ? 4u : degree;
}
#endif
