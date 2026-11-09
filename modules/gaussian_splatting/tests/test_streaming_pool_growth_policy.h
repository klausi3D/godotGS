#pragma once

#include "test_macros.h"

// [VRAMBudgetRegulator] is the strict lane of run_module_tests.py: a case only under an unlisted tag lands in the advisory
// [untagged] net, where a failure cannot fail CI.

#include "../core/streaming_pool_growth_policy.h"

namespace StreamingPoolGrowthPolicyTests {

using namespace gs_streaming_pool_growth;

static constexpr uint64_t kMiB = uint64_t(1) << 20;
static constexpr uint64_t kGiB = uint64_t(1) << 30;
// One atlas page: 1,024 splats x 176 B (run 42644237192: 23,808 pages = 4092 MiB).
static constexpr uint64_t kPageBytes = 1024u * 176u;
static constexpr uint64_t kPagesPerMaxChunk = 64;

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][StreamingPoolGrowth] a world larger than the pool grows to the ceiling at once") {
	// Tier A: 201 M splats registered, the pool starts at 256 pages and is capped at 23,808 (32-bit addressing).
	GrowInputs in;
	in.current_pages = 256;
	in.ceiling_pages = 23808;
	in.budget_max_pages = 23806;
	in.needed_pages = needed_pages_with_headroom(196608, kPagesPerMaxChunk);
	in.page_bytes = kPageBytes;
	in.device_free_known = true;
	in.device_free_bytes = 8 * kGiB;
	// Doubling (512) would walk 512 -> ... -> 16384 -> 23808 and peak at 2816 + 4092 MiB in the last grow.
	CHECK(compute_grow_target(in) == 23806);

	// A scene that fits keeps doubling (the needed pages are below 2x current).
	in.needed_pages = needed_pages_with_headroom(100, kPagesPerMaxChunk);
	CHECK(compute_grow_target(in) == 512);
	in.needed_pages = 0; // unknown
	CHECK(compute_grow_target(in) == 512);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][StreamingPoolGrowth] a grow never allocates more than the free device-local memory") {
	GrowInputs in;
	in.current_pages = 16384; // 2816 MiB, still alive while the new buffer is allocated
	in.ceiling_pages = 23808;
	in.needed_pages = 400000;
	in.page_bytes = kPageBytes;
	in.device_free_known = true;

	// 4 GiB free: (4096 - 256 MiB margin) / 176 KiB = 22,341 pages, not the 23,808 ceiling.
	in.device_free_bytes = 4 * kGiB;
	CHECK(compute_grow_target(in) == uint32_t((4 * kGiB - kDeviceFreeMarginBytes) / kPageBytes));
	CHECK(compute_grow_target(in) == 22341);

	// 3 GiB free: room for 16,384 pages = no larger than the pool, so no grow.
	in.device_free_bytes = 3 * kGiB;
	CHECK(compute_grow_target(in) == 0);
	in.device_free_bytes = 0;
	CHECK(compute_grow_target(in) == 0);

	// Unknown free memory does not clamp (the backend cannot tell).
	in.device_free_known = false;
	CHECK(compute_grow_target(in) == 23808);

	// The budget clamp still applies, and no grow happens at the ceiling.
	in.budget_max_pages = 20000;
	CHECK(compute_grow_target(in) == 20000);
	in.current_pages = 23808;
	CHECK(compute_grow_target(in) == 0);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][StreamingPoolGrowth] free device memory prefers VK_EXT_memory_budget") {
	uint64_t free_bytes = 1;
	// The driver's budget accounts for other processes: budget - usage.
	CHECK(device_free_bytes(true, 10 * kGiB, 1 * kGiB, true, 9 * kGiB, 6 * kGiB, free_bytes));
	CHECK(free_bytes == 3 * kGiB);
	CHECK(device_free_bytes(true, 10 * kGiB, 1 * kGiB, true, 5 * kGiB, 6 * kGiB, free_bytes));
	CHECK(free_bytes == 0);
	// Without the extension: 80 % of the heap minus this process's allocator blocks.
	CHECK(device_free_bytes(true, 10 * kGiB, 7 * kGiB, false, 0, 0, free_bytes));
	CHECK(free_bytes == 10 * kGiB / 100 * 80 - 7 * kGiB);
	// Nothing reported: unknown, never "plenty".
	CHECK_FALSE(device_free_bytes(false, 0, 0, false, 0, 0, free_bytes));
	CHECK(free_bytes == 0);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][StreamingPoolGrowth] a grow keeps only a device-local or unknown placement") {
	CHECK(grow_keeps_allocation(true, true));
	CHECK_FALSE(grow_keeps_allocation(true, false));
	CHECK(grow_keeps_allocation(false, false));
	CHECK(grow_keeps_allocation(false, true));
}

} // namespace StreamingPoolGrowthPolicyTests
