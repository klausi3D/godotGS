#ifndef GS_TILE_ORDER_CONTRACT_H
#define GS_TILE_ORDER_CONTRACT_H

// #1544: total order of the overlap (tile) sort.
//
// The overlap sort key is (tile, depth, tie). The tie holds only the low GS_SORT_TIE_BITS (<= 16) bits
// of global_idx (tile_binning.glsl gs_pack_sort_key), and EMIT takes each record's slot from
// atomicAdd, so records of one tile with equal full keys leave the sort in emit order, which varies
// from frame to frame. Pass 4 of tile_prefix_scan.glsl runs on the sorted arrays inside the overlap
// sort's command list and orders every run of equal keys by global_idx. The raster order is then
// (key, global_idx): total, and independent of the emit slots and of the sort's stability.

#include <cstddef>
#include <cstdint>

#ifdef TESTS_ENABLED
#include "core/string/ustring.h"
#include <atomic>
#endif

namespace GaussianSplatting {

// Longest run of equal keys pass 4 orders. With 64-bit keys and 16 tie bits a run needs 2^16
// equal-depth splats per extra member, so 256 covers 2^24 visible splats. Longer runs are left as
// the sort produced them and counted in key_overflow (tile_order_key_overflow), never truncated.
static constexpr uint32_t kTileOrderRunCap = 256u;

// Host mirror of OrderStats (tile_prefix_scan.glsl pass 4, set 0 binding 3). Cumulative counters;
// the renderer never clears them.
struct TileOrderStatsGPU {
	uint32_t runs_resolved = 0; // runs of equal keys ordered by value
	uint32_t runs_reordered = 0; // runs the sort left out of global_idx order (counted before ordering)
	uint32_t key_overflow = 0; // tile_order_key_overflow: runs longer than the run cap, left unresolved
	uint32_t equal_key_pairs = 0; // sort_equal_key_adjacent_pairs: adjacent records with equal full keys
	uint32_t equal_depth_pairs = 0; // tile_equal_depth_adjacent_pairs: adjacent records with equal (tile, depth)
	uint32_t truncated_tie_equal = 0; // test mode TIE_TRUNCATED_16 only
	uint32_t passes = 0; // dispatches of pass 4
	uint32_t reserved = 0;
};
static_assert(sizeof(TileOrderStatsGPU) == 32, "TileOrderStatsGPU must match the 32-byte OrderStats SSBO");
static_assert(offsetof(TileOrderStatsGPU, key_overflow) == 8, "key_overflow is the third OrderStats word");
static_assert(offsetof(TileOrderStatsGPU, passes) == 24, "passes is the seventh OrderStats word");

// Host mirror of OrderControl (tile_prefix_scan.glsl pass 4 push constant).
struct TileOrderControlGPU {
	uint32_t key_words = 2; // 1: 32-bit keys, 2: 64-bit keys
	uint32_t tie_bits = 0; // low key bits holding the tie
	uint32_t run_cap = kTileOrderRunCap;
	uint32_t test_mode = 0; // TileOrderTestMode; the shader reads it only in test builds
};
static_assert(sizeof(TileOrderControlGPU) == 16, "TileOrderControlGPU must match the 16-byte push constant");

enum TileOrderTestMode : uint32_t {
	TILE_ORDER_TEST_MODE_PRODUCTION = 0u,
	TILE_ORDER_TEST_MODE_PASS_DISABLED = 1u, // control: no run is ordered
	TILE_ORDER_TEST_MODE_TIE_TRUNCATED_16 = 2u, // control: runs ordered by the low 16 bits of global_idx only
};

// Bits of the overlap sort key that hold the tie (the low bits of global_idx), per layout. Single source
// for the binning define GS_SORT_TIE_BITS and for pass 4. 32-bit keys carry no tie bits here: the
// binning's 8-bit quantized-path tie is compiled only when GS_SORT_TIE_BITS > 0, which this returns
// only for keys wider than 32 bits.
inline uint32_t tile_order_tie_bits(uint32_t p_key_bits, uint32_t p_required_tile_bits) {
	if (p_key_bits <= 32u) {
		return 0u;
	}
	const uint32_t tile_bits = p_required_tile_bits < 32u ? p_required_tile_bits : 32u;
	const uint32_t spare_bits = tile_bits < 32u ? 32u - tile_bits : 0u;
	return spare_bits < 16u ? spare_bits : 16u;
}

} // namespace GaussianSplatting

#ifdef TESTS_ENABLED
// Test-only forcing hooks for the #1544 fixture and its controls. Change only between frames.
namespace GSTileOrderTestControl {
// EMIT reverses each tile's atomicAdd slots on odd frames (debug_audit.frame_id), a forced
// per-frame permutation of the emit order. Compiled into the binning shader only while set.
inline std::atomic<bool> permute_emit_order{ false };
// TileOrderTestMode passed to pass 4.
inline std::atomic<uint32_t> order_test_mode{ GaussianSplatting::TILE_ORDER_TEST_MODE_PRODUCTION };
// 0: kTileOrderRunCap. Otherwise the run cap pass 4 uses (forces tile_order_key_overflow).
inline std::atomic<uint32_t> run_cap_override{ 0u };

inline uint32_t shader_variant() { return uint32_t(permute_emit_order.load()); }
inline String binning_shader_defines() {
	return permute_emit_order.load() ? String("#define GS_TEST_TILE_EMIT_ORDER_PERMUTE 1\n") : String();
}
inline void reset() {
	permute_emit_order.store(false);
	order_test_mode.store(GaussianSplatting::TILE_ORDER_TEST_MODE_PRODUCTION);
	run_cap_override.store(0u);
}
} // namespace GSTileOrderTestControl
#endif

#endif // GS_TILE_ORDER_CONTRACT_H
