#include "gs_semantic.glsl"
#ifndef GS_INSTANCE_LAYOUT_GLSL
#define GS_INSTANCE_LAYOUT_GLSL

#include "gs_splat_ref_encoding.glsl"
#include "gs_hlod_page_constants.glsl"

// Instance pipeline layout (always active).

#ifndef GS_MAX_ASSET_LODS
#define GS_MAX_ASSET_LODS 8
#endif

// BV-04b: records per splat per HLOD level (HlodLevelRecordStats, binding 24 of the HLOD emit pass).
// Mirrors GS_HLOD_LEVEL_STAT_SLOTS / GS_DRAWN_NODE_LEVEL_NONE in renderer/gaussian_gpu_layout.h.
#define GS_HLOD_LEVEL_STAT_SLOTS 16
#define GS_DRAWN_NODE_LEVEL_NONE 0xFFFFFFFFu

uint gs_hlod_level_stat_slot(uint level) {
    if (level == GS_DRAWN_NODE_LEVEL_NONE) {
        return uint(GS_HLOD_LEVEL_STAT_SLOTS - 1);
    }
    return min(level, uint(GS_HLOD_LEVEL_STAT_SLOTS - 2));
}

// HLOD level debug view (RenderParams.debug_hlod_level_tint). Palette by node height:
// 0 (leaf) green, 1 blue, 2 yellow, 3 orange, 4 red, 5+ purple. The splat's own luminance
// modulates the tint so structure stays readable. Object draws (GS_DRAWN_NODE_LEVEL_NONE)
// keep their colour.
vec3 gs_hlod_level_tint_color(vec3 p_color, uint p_level) {
    if (p_level == GS_DRAWN_NODE_LEVEL_NONE) {
        return p_color;
    }
    const vec3 palette[6] = vec3[6](vec3(0.10, 0.80, 0.20), vec3(0.20, 0.60, 1.00), vec3(1.00, 0.85, 0.10),
            vec3(1.00, 0.50, 0.00), vec3(0.95, 0.15, 0.15), vec3(0.80, 0.20, 0.90));
    float luma = clamp(dot(p_color, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    return palette[min(p_level, 5u)] * (0.35 + 0.65 * luma);
}

#ifndef GS_MAX_SPHERE_EFFECTORS
#define GS_MAX_SPHERE_EFFECTORS 4
#endif

// Per-instance data (std430, 16-byte aligned).
struct InstanceDataGPU {
    vec4 rotation;           // quat (x,y,z,w)
    vec4 inv_rotation;       // quat inverse
    vec4 translation_scale;  // xyz = translation, w = uniform_scale
    vec4 params;             // x = opacity, y = lod_bias, z = wind_intensity, w = wind_mode
    uvec2 ids;               // x = asset_id, y = flags plus cached A2 sort metadata
    uvec2 lod;               // x = resolved_lod_level, y = region override segment base (0xFFFFFFFF = none)
    vec4 wind_params;        // xyz = wind direction override (0,0,0=infer global), w = wind frequency scale
    vec4 effect_params;      // x = position response scale, y = opacity response scale,
                             // z = scene effector mask bits, w = bound scene effector count snapshot
    uvec2 placement_key;     // low/high halves of stable placement ObjectID; world row = 0
    uvec2 placement_pad;     // x = region segment length, y = dense-remap generation
};

// Instance flag bits (ids.y).
const uint GS_INSTANCE_FLAG_IS_2D = 1u << 0u;
const uint GS_INSTANCE_FLAG_ROTATION_IDENTITY = 1u << 1u;
const uint GS_INSTANCE_FLAG_SCALE_IDENTITY = 1u << 2u;
const uint GS_INSTANCE_FLAG_TRANSLATION_ZERO = 1u << 3u;
// Mip-Splatting opacity compensation for assets trained with antialiasing (#1173).
// Mirrors GS_INSTANCE_FLAG_ANTIALIASED in renderer/gaussian_gpu_layout.h.
const uint GS_INSTANCE_FLAG_ANTIALIASED = 1u << 4u;
const uint GS_INSTANCE_FLAG_SHADOW_PASS = 1u << 5u;
// Mirrors gaussian_gpu_layout.h; these fields do not change the 128 B stride.
const uint GS_INSTANCE_FLAGS_MASK = 0x3fu;
const uint GS_INSTANCE_SORT_LOCAL_SHIFT = 6u;
const uint GS_INSTANCE_SORT_LOCAL_MASK = 0x7c0u;
const uint GS_INSTANCE_SORT_SINGLE_CHUNK = 0x800u;
const uint GS_INSTANCE_SORT_PREFIX_MASK = 0xffffc000u;
const uint GS_INSTANCE_TEST_OVERRIDE_BY_ADDRESS = 1u << 12u;
const uint GS_INSTANCE_TEST_HIDE_ONLY_IN_RASTER = 1u << 13u;


// Asset/chunk metadata flags.
const uint GS_ASSET_FLAG_IS_2D = 1u << 0u;
const uint GS_ASSET_FLAG_DC_LINEAR_RGB = 1u << 1u;

// Instance wind override modes (params.w).
#ifndef GS_INSTANCE_WIND_MODE_INHERIT
#define GS_INSTANCE_WIND_MODE_INHERIT 0u
#define GS_INSTANCE_WIND_MODE_FORCE_DISABLED 1u
#define GS_INSTANCE_WIND_MODE_FORCE_ENABLED 2u
#define GS_INSTANCE_WIND_MODE_SEMANTIC_INHERIT 3u
#endif

// Per-asset metadata table.
struct AssetLodRangeGPU {
    uint base;               // base into AssetChunkIndexBuffer
    uint count;              // number of chunk indices for this LOD
};

struct AssetMetaGPU {
    uint lod_count;
    uint sh_degree;          // max SH bands for asset
    uint flags;              // GS_ASSET_FLAG_* metadata shared with chunk flags
    uint pad0;

    vec3 bounds_center_local;
    float bounds_radius_local;

    uint chunk_index_base;   // base into AssetChunkIndexBuffer (all LODs)
    uint chunk_index_count;  // total indices across all LODs
    uint quant_chunk_base;   // base into QuantizationChunkBuffer (asset-wide range)
    uint quant_chunk_count;  // total quant chunks for asset (informational)

    AssetLodRangeGPU lod_ranges[GS_MAX_ASSET_LODS];
};

// Global chunk metadata table (asset-local bounds).
struct ChunkMetaGPU {
    uint atlas_base;         // start index into global atlas buffer
    uint splat_count;
    uint quant_base;         // absolute base into QuantizationChunkBuffer (authoritative)
    uint quant_count;

    vec3 bounds_center_local;
    float bounds_radius_local;

    uint asset_id;
    uint lod_level;
    uint flags;
    uint sh_limit;           // per-chunk SH band limit (optional)
    uint page_table_ref;     // 0: contiguous; otherwise forward-table base + 1.
    uint reserved_node_y; // Baked node id / asset-local chunk id, independent of atlas placement.
    uint reserved_node_z; // World local-index bit width; bit 8 = canonical singleton.
};

// Global chunk index indirection (dense).
struct AssetChunkIndexGPU {
    uint chunk_id;           // index into ChunkMetaGPU[]
};

// Visible chunk list emitted by Stage A.
struct VisibleChunkRefGPU {
    uint instance_id;
    uint chunk_id;
};

// Per-draw record of the HLOD route (replaces VisibleChunkRefGPU there; written by the CPU).
// The first two words keep VisibleChunkRefGPU's order. camera_offset is view dependent and
// therefore lives only here, never in the shared ChunkMetaGPU payload.
struct DrawnNodeGPU {
    uint instance_id;
    uint chunk_id;
    float fade;              // Reserved for the S6 cross-fade; written as 1.0 and not read yet
    uint level;              // BV-04b: HLOD level (node height, 0 = leaf); GS_DRAWN_NODE_LEVEL_NONE for objects
    vec4 camera_offset;      // xyz = R s c + T - camera (double on the CPU, float last), w = 0
};

// Visible splat list emitted by Stage B.
struct SplatRefGPU {
    uint instance_id;
    uint atlas_index;
};

// Per-instance color grading (std430), indexed by SplatRefGPU.instance_id.
// primary:   x = enabled (0/1), y = exposure, z = contrast, w = saturation
// secondary: x = temperature,   y = tint,     z = hue_shift, w = reserved
struct InstanceGradingGPU {
    vec4 primary;
    vec4 secondary;
};

const uint GS_INSTANCE_DATA_GPU_SIZE = 128u;
const uint GS_ASSET_LOD_RANGE_GPU_SIZE = 8u;
const uint GS_ASSET_META_GPU_SIZE = 48u + 8u * GS_MAX_ASSET_LODS;
const uint GS_CHUNK_META_GPU_SIZE = 64u;
const uint GS_ASSET_CHUNK_INDEX_GPU_SIZE = 4u;
const uint GS_VISIBLE_CHUNK_REF_GPU_SIZE = 8u;
const uint GS_DRAWN_NODE_GPU_SIZE = 32u;
const uint GS_SPLAT_REF_GPU_SIZE = 8u;
const uint GS_INSTANCE_GRADING_GPU_SIZE = 32u;

#endif // GS_INSTANCE_LAYOUT_GLSL
