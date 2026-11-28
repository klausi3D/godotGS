#[compute]

#version 450

#include "includes/platform_compat.glsl"

#ifndef TILE_SIZE
#define TILE_SIZE GS_TILE_SIZE
#endif

#ifndef GS_PREFIX_LOCAL_SIZE
#define GS_PREFIX_LOCAL_SIZE 256
#endif

#ifndef GS_TILE_PREFIX_PASS2_OP_INCLUSIVE_STEP
#define GS_TILE_PREFIX_PASS2_OP_INCLUSIVE_STEP 0
#endif

#ifndef GS_TILE_PREFIX_PASS2_OP_EXCLUSIVE_SHIFT
#define GS_TILE_PREFIX_PASS2_OP_EXCLUSIVE_SHIFT 1
#endif

#ifndef GS_TILE_PREFIX_PASS2_OP_COPY
#define GS_TILE_PREFIX_PASS2_OP_COPY 2
#endif

#ifndef GS_TILE_PREFIX_PASS2_SOURCE_WG_SUMS
#define GS_TILE_PREFIX_PASS2_SOURCE_WG_SUMS 0
#endif

#ifndef GS_TILE_PREFIX_PASS2_SOURCE_WG_OFFSETS
#define GS_TILE_PREFIX_PASS2_SOURCE_WG_OFFSETS 1
#endif

layout(local_size_x = GS_PREFIX_LOCAL_SIZE, local_size_y = 1, local_size_z = 1) in;

#ifndef GS_TILE_PREFIX_PASS_4
layout(set = 0, binding = 0, std430) readonly buffer TileCounts {
    uint counts[];
} tile_counts;

layout(set = 0, binding = 1, std430) buffer TileRanges {
    uvec2 ranges[];
} tile_ranges;

// Workgroup partial sums (one uint per workgroup)
layout(set = 0, binding = 2, std430) buffer WorkgroupSums {
    uint wg_sums[];
} wg_sums;

// Scanned workgroup offsets (same length as wg_sums)
layout(set = 0, binding = 3, std430) buffer WorkgroupOffsets {
    uint wg_offsets[];
} wg_offsets;

layout(set = 0, binding = 4, std430) buffer PrefixTotal {
    uint total;
} prefix_total;

// GPU-driven dispatch/count with overflow detection (matches IndirectDispatchLayout).
layout(set = 0, binding = 5, std430) buffer IndirectDispatch {
    uint dispatch_xyz[3];   // offset 0: dispatch groups for downstream stages
    uint element_count;     // offset 12: clamped to capacity
    uint overflow_flag;     // offset 16: 1 if total > capacity, 0 otherwise
    uint unclamped_total;   // offset 20: raw total before clamping
} indirect_dispatch;

layout(set = 1, binding = 0, std140) uniform PrefixParams {
    uint total_tiles;
    uint workgroup_stride;
    uint total_workgroups;
    uint global_sort_capacity;  // Used for overflow detection in Pass 3
} params;
#endif // !GS_TILE_PREFIX_PASS_4

#ifdef GS_TILE_PREFIX_PASS_1
// Pass 1: local exclusive scan + write wg_sums and base ranges.
shared uint scratch[GS_PREFIX_LOCAL_SIZE];
// PERF-7 (#678): Shared flag to track if any thread in workgroup has non-zero count
shared uint wg_has_nonzero;

// Compute pass 1: local exclusive scan and workgroup totals.
void main() {
    uint lid = gl_LocalInvocationID.x;
    uint gid = gl_WorkGroupID.x;
    uint global_idx = gid * GS_PREFIX_LOCAL_SIZE + lid;

    uint count = (global_idx < params.total_tiles) ? tile_counts.counts[global_idx] : 0;
    scratch[lid] = count;

    // PERF-7 (#678): Early-out optimization for empty workgroups
    // Initialize shared flag and check if all tiles in workgroup are empty
    if (lid == 0u) {
        wg_has_nonzero = 0u;
    }
    barrier();

    // Atomically set flag if this thread has non-zero count
    if (count > 0u) {
        atomicOr(wg_has_nonzero, 1u);
    }
    barrier();

    // If entire workgroup is empty, write zeros and skip expensive scan
    if (wg_has_nonzero == 0u) {
        if (lid == 0u) {
            wg_sums.wg_sums[gid] = 0u;
        }
        if (global_idx < params.total_tiles) {
            tile_ranges.ranges[global_idx] = uvec2(0u, 0u);
        }
        return;
    }

    // Upsweep
    for (uint stride = 1u; stride < GS_PREFIX_LOCAL_SIZE; stride <<= 1u) {
        uint idx = (lid + 1u) * stride * 2u - 1u;
        if (idx < GS_PREFIX_LOCAL_SIZE) {
            scratch[idx] += scratch[idx - stride];
        }
        barrier();
    }

    if (lid == 0u) {
        wg_sums.wg_sums[gid] = scratch[GS_PREFIX_LOCAL_SIZE - 1u];
        scratch[GS_PREFIX_LOCAL_SIZE - 1u] = 0u; // clear for downsweep
    }
    barrier();

    // Downsweep
    for (uint stride = GS_PREFIX_LOCAL_SIZE >> 1u; stride > 0u; stride >>= 1u) {
        uint idx = (lid + 1u) * stride * 2u - 1u;
        if (idx < GS_PREFIX_LOCAL_SIZE) {
            uint tmp = scratch[idx - stride];
            scratch[idx - stride] = scratch[idx];
            scratch[idx] += tmp;
        }
        barrier();
    }

    if (global_idx < params.total_tiles) {
        tile_ranges.ranges[global_idx] = uvec2(scratch[lid], count);
    }
}
#endif // GS_TILE_PREFIX_PASS_1

#ifdef GS_TILE_PREFIX_PASS_2
// Pass 2: scalable multi-level scan over wg_sums/wg_offsets.
layout(push_constant, std430) uniform PrefixPass2Control {
    uint operation;
    uint source_buffer;
    uint stride;
    uint reserved;
} pass2_control;

// Read the current prefix-scan source buffer for pass 2.
uint read_pass2_source(uint idx) {
    if (pass2_control.source_buffer == GS_TILE_PREFIX_PASS2_SOURCE_WG_OFFSETS) {
        return wg_offsets.wg_offsets[idx];
    }
    return wg_sums.wg_sums[idx];
}

// Write the current prefix-scan destination buffer for pass 2.
void write_pass2_dest(uint idx, uint value) {
    // Destination is the opposite buffer of source_buffer to keep ping-pong deterministic.
    if (pass2_control.source_buffer == GS_TILE_PREFIX_PASS2_SOURCE_WG_OFFSETS) {
        wg_sums.wg_sums[idx] = value;
    } else {
        wg_offsets.wg_offsets[idx] = value;
    }
}

// Compute pass 2: multi-level scan or buffer copy for workgroup offsets.
void main() {
    uint global_idx = gl_GlobalInvocationID.x;
    if (global_idx >= params.total_workgroups) {
        return;
    }

    uint result = 0u;
    if (pass2_control.operation == GS_TILE_PREFIX_PASS2_OP_INCLUSIVE_STEP) {
        uint value = read_pass2_source(global_idx);
        uint addend = 0u;
        if (pass2_control.stride > 0u && global_idx >= pass2_control.stride) {
            addend = read_pass2_source(global_idx - pass2_control.stride);
        }
        result = value + addend;
    } else if (pass2_control.operation == GS_TILE_PREFIX_PASS2_OP_EXCLUSIVE_SHIFT) {
        result = (global_idx == 0u) ? 0u : read_pass2_source(global_idx - 1u);
    } else { // GS_TILE_PREFIX_PASS2_OP_COPY
        result = read_pass2_source(global_idx);
    }

    write_pass2_dest(global_idx, result);
}
#endif // GS_TILE_PREFIX_PASS_2

#ifdef GS_TILE_PREFIX_PASS_3
// Pass 3: add workgroup offsets into base ranges.
void main() {
    uint lid = gl_LocalInvocationID.x;
    uint gid = gl_WorkGroupID.x;
    uint global_idx = gid * GS_PREFIX_LOCAL_SIZE + lid;

    if (global_idx >= params.total_tiles) {
        return;
    }

    uint base_offset = (gid < params.total_workgroups) ? wg_offsets.wg_offsets[gid] : 0u;
    uvec2 range = tile_ranges.ranges[global_idx];
    range.x += base_offset;
    tile_ranges.ranges[global_idx] = range;

    // Last tile writes the total overlap count and performs overflow detection.
    if (global_idx == params.total_tiles - 1u) {
        uint total = range.x + range.y;
        prefix_total.total = total;

        // Store unclamped total for async readback (capacity expansion decisions)
        indirect_dispatch.unclamped_total = total;

        // Overflow detection: compare against capacity
        bool overflow = (total > params.global_sort_capacity) && (params.global_sort_capacity > 0u);
        indirect_dispatch.overflow_flag = overflow ? 1u : 0u;

        // Clamp element_count to capacity for downstream GPU stages
        uint clamped_total = overflow ? params.global_sort_capacity : total;
        indirect_dispatch.element_count = clamped_total;

        // Populate dispatch groups based on clamped count
        uint groups_x = (clamped_total + GS_PREFIX_LOCAL_SIZE - 1u) / GS_PREFIX_LOCAL_SIZE;
        indirect_dispatch.dispatch_xyz[0] = max(groups_x, 1u);
        indirect_dispatch.dispatch_xyz[1] = 1u;
        indirect_dispatch.dispatch_xyz[2] = 1u;
    }
}
#endif // GS_TILE_PREFIX_PASS_3

#ifdef GS_TILE_PREFIX_PASS_4
// Pass 4 (#1544): total order of the sorted overlap records.
//
// The overlap sort key is (tile, depth, tie) with a tie of at most GS_SORT_TIE_BITS (<= 16) low bits of
// global_idx, and EMIT takes each record's slot from atomicAdd. Two records of one tile whose full keys
// are equal (bitwise-equal depth, global_idx equal modulo 2^tie_bits) therefore leave the sort in emit
// order, which varies from frame to frame. This pass runs on the sorted arrays, inside the overlap
// sort's command list, and orders every run of equal keys by its value (global_idx, unique within a
// tile). The result is the lexicographic order (key, global_idx): a total order that depends neither
// on the emit slots nor on the stability of the sort. Keys are not modified.
//
// A run longer than run_cap is left as it is and counted in key_overflow (tile_order_key_overflow):
// refused and reported, never resolved by a truncated tie. With 64-bit keys and 16 tie bits a run
// needs 2^16 equal-depth splats per extra member, so run_cap = 256 covers 2^24 visible splats.
layout(set = 0, binding = 0, std430) restrict readonly buffer OrderKeys {
    uint words[];
} order_keys;

layout(set = 0, binding = 1, std430) restrict buffer OrderValues {
    uint values[];
} order_values;

// IndirectDispatchLayout (written by pass 3): element_count is the sorted record count.
layout(set = 0, binding = 2, std430) restrict readonly buffer OrderCount {
    uint dispatch_xyz[3];
    uint element_count;
    uint overflow_flag;
    uint unclamped_total;
} order_count;

// TileOrderStatsGPU (renderer/tile_order_contract.h). Cumulative; the renderer never clears it.
layout(set = 0, binding = 3, std430) restrict buffer OrderStats {
    uint runs_resolved;        // runs of equal keys this pass ordered by value
    uint runs_reordered;       // runs the sort left out of global_idx order (counted before ordering)
    uint key_overflow;         // tile_order_key_overflow: runs longer than run_cap, left unresolved
    uint equal_key_pairs;      // sort_equal_key_adjacent_pairs: adjacent records with equal full keys
    uint equal_depth_pairs;    // tile_equal_depth_adjacent_pairs: adjacent records with equal (tile, depth)
    uint truncated_tie_equal;  // test mode 2 only: runs in which the truncated tie compared two values equal
    uint passes;               // dispatches of this pass
    uint reserved;
} order_stats;

// TileOrderControlGPU (renderer/tile_order_contract.h).
layout(push_constant, std430) uniform OrderControl {
    uint key_words;  // 1 (32-bit keys) or 2 (64-bit keys, .x = low word)
    uint tie_bits;   // low key bits that hold the tie (0: none)
    uint run_cap;    // longest run this pass orders
    uint test_mode;  // 0 in production; read only in test builds: 1 = pass disabled, 2 = tie truncated to 16 bits
} order_control;

uvec2 gs_order_key(uint i) {
    if (order_control.key_words == 2u) {
        return uvec2(order_keys.words[2u * i], order_keys.words[2u * i + 1u]);
    }
    return uvec2(order_keys.words[i], 0u);
}

// (tile, depth): the key without its tie bits.
uvec2 gs_order_key_without_tie(uvec2 key) {
    uint s = order_control.tie_bits;
    if (s == 0u) {
        return key;
    }
    if (order_control.key_words == 2u) {
        return uvec2((key.x >> s) | (key.y << (32u - s)), key.y >> s);
    }
    return uvec2(key.x >> s, 0u);
}

uint gs_order_rank_value(uint value) {
#ifdef GS_TESTS_ENABLED
    if (order_control.test_mode == 2u) {
        return value & 0xFFFFu;
    }
#endif
    return value;
}

void gs_order_record(uint i, uint n) {
    uvec2 key = gs_order_key(i);
    bool same_as_previous = false;
    if (i > 0u) {
        uvec2 previous = gs_order_key(i - 1u);
        same_as_previous = all(equal(previous, key));
        if (same_as_previous) {
            atomicAdd(order_stats.equal_key_pairs, 1u);
        }
        if (all(equal(gs_order_key_without_tie(previous), gs_order_key_without_tie(key)))) {
            atomicAdd(order_stats.equal_depth_pairs, 1u);
        }
    }
    // Only the first record of a run of equal keys works on it. Runs are disjoint, so no two
    // invocations write the same value and no invocation reads a value another one writes.
    if (same_as_previous || i + 1u >= n || !all(equal(gs_order_key(i + 1u), key))) {
        return;
    }
    uint run_length = 2u;
    while (i + run_length < n && run_length <= order_control.run_cap && all(equal(gs_order_key(i + run_length), key))) {
        run_length += 1u;
    }
    if (run_length > order_control.run_cap) {
        atomicAdd(order_stats.key_overflow, 1u);
        return;
    }
    // runs_reordered: the sort left this run out of global_idx order (measured before any reordering).
    for (uint a = 1u; a < run_length; ++a) {
        if (order_values.values[i + a - 1u] > order_values.values[i + a]) {
            atomicAdd(order_stats.runs_reordered, 1u);
            break;
        }
    }
#ifdef GS_TESTS_ENABLED
    if (order_control.test_mode == 1u) {
        return;
    }
#endif
    // Insertion sort by value. Values are unique within a tile, so the result does not depend on
    // the order in which the sort left the run.
    bool truncated_equal = false;
    for (uint a = 1u; a < run_length; ++a) {
        uint value = order_values.values[i + a];
        uint rank = gs_order_rank_value(value);
        uint b = a;
        while (b > 0u) {
            uint other_rank = gs_order_rank_value(order_values.values[i + b - 1u]);
            if (other_rank == rank) {
                truncated_equal = true;
            }
            if (other_rank <= rank) {
                break;
            }
            order_values.values[i + b] = order_values.values[i + b - 1u];
            b -= 1u;
        }
        order_values.values[i + b] = value;
    }
    atomicAdd(order_stats.runs_resolved, 1u);
    if (truncated_equal) {
        atomicAdd(order_stats.truncated_tie_equal, 1u);
    }
}

// Dispatched directly (not indirectly: the indirect buffer is bound as storage in this compute list)
// over min(capacity, 65535 groups); a grid-stride loop covers element_count.
void main() {
    uint n = order_count.element_count;
    uint stride = gl_NumWorkGroups.x * uint(GS_PREFIX_LOCAL_SIZE);
    if (gl_GlobalInvocationID.x == 0u) {
        atomicAdd(order_stats.passes, 1u);
    }
    for (uint i = gl_GlobalInvocationID.x; i < n; i += stride) {
        gs_order_record(i, n);
    }
}
#endif // GS_TILE_PREFIX_PASS_4
