/**************************************************************************/
/*  test_overlap_bootstrap_policy.h                                       */
/*  BV-10: demand-driven overlap bootstrap, old + new pre-flight, E4.     */
/**************************************************************************/

#ifndef GAUSSIAN_SPLATTING_TEST_OVERLAP_BOOTSTRAP_POLICY_H
#define GAUSSIAN_SPLATTING_TEST_OVERLAP_BOOTSTRAP_POLICY_H

#include "test_macros.h"

#include "../renderer/gpu_sorting_constants.h"
#include "../renderer/overlap_bootstrap_policy.h"

namespace TestOverlapBootstrapPolicy {

constexpr uint64_t MIB = 1024u * 1024u;
constexpr uint64_t GIB = 1024u * MIB;

inline uint64_t default_sort_bytes(uint64_t p_records) {
	return GPUSortingConstants::overlap_sort_total_bytes(p_records, 4u, 512u, 64u);
}

TEST_CASE("[GaussianSplatting][Config][OverlapBootstrap] E4 accounting is the ADR's 28 B per record") {
	// The ADR's two figures: 100M records 2,670 MiB, 32M records 855 MiB.
	CHECK(default_sort_bytes(100000000u) / MIB == 2670u);
	CHECK(default_sort_bytes(32000000u) / MIB == 854u);
	// 8 + 4 primary, 8 + 4 temp, 2 histogram, 2 workgroup prefix.
	CHECK(default_sort_bytes(1u << 20) / (1u << 20) == 28u);
	// Key width and radix change the bill.
	CHECK(GPUSortingConstants::overlap_sort_total_bytes(1u << 20, 4u, 512u, 32u) / (1u << 20) == 18u);
	CHECK(GPUSortingConstants::overlap_sort_total_bytes(1u << 20, 8u, 512u, 64u) / (1u << 20) == 56u);
	CHECK(default_sort_bytes(0u) == 0u);
	CHECK(GPUSortingConstants::overlap_sort_total_bytes(1u << 20, 5u, 512u, 64u) == GPUSortingConstants::SORT_PATH_SIZE_UNSUPPORTED);
}

TEST_CASE("[GaussianSplatting][Config][OverlapBootstrap] the unmeasured bootstrap costs the steady figure, not the 2,670 MiB cap") {
	const uint32_t seed = GaussianSplatting::overlap_demand_estimate(4000000u, 0u);
	CHECK(seed == 32000000u);
	CHECK(default_sort_bytes(seed) / MIB == 854u);
	CHECK(GaussianSplatting::overlap_demand_estimate(0u, 0u) == 0u);
}

TEST_CASE("[GaussianSplatting][Config][OverlapBootstrap] a measurement replaces the seed in both directions") {
	const uint32_t sparse_q8 = GaussianSplatting::overlap_records_per_splat_q8(1000000u, 4000000u);
	CHECK(sparse_q8 == 64u); // 0.25 records/splat survives the fixed point.
	CHECK(GaussianSplatting::overlap_demand_estimate(4000000u, sparse_q8) == 1203125u);

	// 100 records/splat close-up: above the seed, and not limited by it.
	const uint32_t close_q8 = GaussianSplatting::overlap_records_per_splat_q8(100000000u, 1000000u);
	CHECK(close_q8 == 25600u);
	const uint32_t close_estimate = GaussianSplatting::overlap_demand_estimate(1000000u, close_q8);
	CHECK(close_estimate >= 120000000u);
	CHECK(close_estimate <= 120004000u);

	// A rise in visible splats leads the budget at the measured rate.
	CHECK(GaussianSplatting::overlap_demand_estimate(2000000u, close_q8) >= 240000000u);

	// An empty view is no sample; saturation stays in range.
	CHECK(GaussianSplatting::overlap_records_per_splat_q8(0u, 4000000u) == 0u);
	CHECK(GaussianSplatting::overlap_records_per_splat_q8(5u, 0u) == 0u);
	CHECK(GaussianSplatting::overlap_records_per_splat_q8(UINT32_MAX, 1u) == UINT32_MAX);
	CHECK(GaussianSplatting::overlap_demand_estimate(UINT32_MAX, UINT32_MAX) == UINT32_MAX);
}

TEST_CASE("[GaussianSplatting][Config][OverlapBootstrap] a grow pads geometrically and never below the request") {
	CHECK(GaussianSplatting::overlap_grow_target(11000000u, 10000000u, 100000000u) == 15000000u);
	CHECK(GaussianSplatting::overlap_grow_target(40000000u, 10000000u, 100000000u) == 40000000u);
	// The padding stops at the hard cap; a request at the cap is served whole.
	CHECK(GaussianSplatting::overlap_grow_target(90000000u, 80000000u, 100000000u) == 100000000u);
	CHECK(GaussianSplatting::overlap_grow_target(100000000u, 90000000u, 100000000u) == 100000000u);
	// A request is never cut, whatever the cap says.
	CHECK(GaussianSplatting::overlap_grow_target(120000000u, 90000000u, 100000000u) == 120000000u);
	// 0 = no cap, and the arithmetic does not wrap.
	CHECK(GaussianSplatting::overlap_grow_target(10u, 3000000000u, 0u) == UINT32_MAX);
}

TEST_CASE("[GaussianSplatting][Config][OverlapBootstrap] the pre-flight charges old + new at the exact boundary") {
	using GaussianSplatting::OverlapResizePreflight;
	const uint64_t device = 10u * GIB;
	const uint64_t limit = device - GaussianSplatting::OVERLAP_RESIZE_DRIVER_RESERVE_BYTES;

	// The used figure holds the old sorter: old + new must fit, new alone is not enough.
	const uint64_t used_with_old = limit - 2u * GIB;
	CHECK(GaussianSplatting::overlap_resize_preflight(used_with_old, 2u * GIB, device) == OverlapResizePreflight::FITS);
	CHECK(GaussianSplatting::overlap_resize_preflight(used_with_old, 2u * GIB + 1u, device) == OverlapResizePreflight::REFUSED);
	// Already above the limit, and a huge request that would wrap a naive sum.
	CHECK(GaussianSplatting::overlap_resize_preflight(limit + 1u, 0u, device) == OverlapResizePreflight::REFUSED);
	CHECK(GaussianSplatting::overlap_resize_preflight(GIB, UINT64_MAX, device) == OverlapResizePreflight::REFUSED);
	// An unsupported radix yields the size sentinel and fails closed.
	CHECK(GaussianSplatting::overlap_resize_preflight(GIB, GPUSortingConstants::SORT_PATH_SIZE_UNSUPPORTED, device) == OverlapResizePreflight::REFUSED);
	// Unknown is never reported as a fit.
	CHECK(GaussianSplatting::overlap_resize_preflight(GIB, GIB, 0u) == OverlapResizePreflight::UNVERIFIED);
	CHECK(GaussianSplatting::overlap_resize_preflight(GIB, GIB, 255u * MIB) == OverlapResizePreflight::UNVERIFIED);
	CHECK(GaussianSplatting::overlap_resize_preflight(0u, GIB, device) == OverlapResizePreflight::UNVERIFIED);
	// A trusted but small device: the reserve swallows the heap, and nothing fits.
	CHECK(GaussianSplatting::overlap_resize_preflight(MIB, MIB, 256u * MIB) == OverlapResizePreflight::REFUSED);
}

} // namespace TestOverlapBootstrapPolicy

#endif // GAUSSIAN_SPLATTING_TEST_OVERLAP_BOOTSTRAP_POLICY_H
