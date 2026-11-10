#ifndef GS_STREAMING_POOL_GROWTH_POLICY_H
#define GS_STREAMING_POOL_GROWTH_POLICY_H

// BV-POOL: how far the streaming persistent buffer (the chunk-route atlas pool) grows, and
// whether an allocation may be kept. Pure (no RenderingDevice), so the policy is testable on
// the host; GaussianStreamingSystem::_try_grow_persistent_buffer_for_atlas_pressure() and
// _grow_persistent_buffer() feed it.
//
// Why: a grow allocates the new buffer while the old one is still alive (the contents are
// copied, and RenderingDevice::free() defers the release). Doubling to the 4 GiB cap therefore
// peaked at old + new = 2.8 + 4.0 GB on a 10 GB card, and the allocator, which only *prefers*
// device-local memory, put the new pool in host memory without an error: every atlas-reading
// pass then ran over PCIe (run 42644237192: 9.4 -> 100 ms at the same pose).
//
// The policy:
//  1. Grow straight to what the registered assets need (the same "asset pages + headroom"
//     sizing initialize() uses), never less than doubling. A world larger than the pool
//     reaches the ceiling in its first grow, while the old buffer is still small.
//  2. Clamp to the growth ceiling and the VRAM budget, as before.
//  3. Clamp to the device-local memory that is actually free (old + new must fit), when the
//     device reports it. Unknown free memory leaves the target unclamped.
//  4. A known non-device-local allocation is never kept by a grow (the old pool stays and
//     growth stops). At initialization there is no older buffer to keep: it is reported loudly.

#include <cstdint>

namespace gs_streaming_pool_growth {

// Safety margin left free on the device-local heap(s) after a grow's allocation.
static constexpr uint64_t kDeviceFreeMarginBytes = uint64_t(256) * 1024 * 1024;
// Without VK_EXT_memory_budget the usable share of the device-local heap is estimated like
// the allocator does (VMA's default budget heuristic, 80 % of the heap).
static constexpr uint64_t kEstimatedUsableHeapPercent = 80;

struct GrowInputs {
	uint32_t current_pages = 0;
	uint32_t ceiling_pages = 0; // Growth ceiling: max capacity and the max_chunks backstop.
	uint64_t budget_max_pages = UINT64_MAX; // VRAM-budget clamp; UINT64_MAX = no clamp.
	uint64_t needed_pages = 0; // Registered asset pages + headroom (0 = unknown).
	uint64_t page_bytes = 0;
	bool device_free_known = false;
	uint64_t device_free_bytes = 0; // Free device-local bytes before the new allocation.
};

// Headroom on top of the registered asset pages, as initialize() sizes it.
inline uint64_t needed_pages_with_headroom(uint64_t p_asset_pages, uint64_t p_pages_per_max_chunk) {
	const uint64_t headroom = p_asset_pages / 4u > 2u * p_pages_per_max_chunk ? p_asset_pages / 4u : 2u * p_pages_per_max_chunk;
	return p_asset_pages + headroom;
}

// Free device-local bytes. Prefers VK_EXT_memory_budget (budget - usage, which accounts for
// other processes); otherwise estimates from the heap size and this process's allocator blocks.
// Returns false when neither is available.
inline bool device_free_bytes(bool p_heap_known, uint64_t p_heap_bytes, uint64_t p_allocator_block_bytes,
		bool p_ext_known, uint64_t p_ext_budget_bytes, uint64_t p_ext_usage_bytes, uint64_t &r_free_bytes) {
	r_free_bytes = 0;
	if (p_ext_known) {
		r_free_bytes = p_ext_budget_bytes > p_ext_usage_bytes ? p_ext_budget_bytes - p_ext_usage_bytes : 0;
		return true;
	}
	if (p_heap_known && p_heap_bytes > 0) {
		const uint64_t usable = p_heap_bytes / 100u * kEstimatedUsableHeapPercent;
		r_free_bytes = usable > p_allocator_block_bytes ? usable - p_allocator_block_bytes : 0;
		return true;
	}
	return false;
}

// The page count to grow to, or 0 when the pool must not grow.
inline uint32_t compute_grow_target(const GrowInputs &p) {
	if (p.current_pages == 0 || p.ceiling_pages <= p.current_pages) {
		return 0;
	}
	uint64_t target = uint64_t(p.current_pages) * 2u;
	if (p.needed_pages > target) {
		target = p.needed_pages;
	}
	if (target > p.ceiling_pages) {
		target = p.ceiling_pages;
	}
	if (target > p.budget_max_pages) {
		target = p.budget_max_pages;
	}
	if (p.device_free_known && p.page_bytes > 0) {
		const uint64_t room = p.device_free_bytes > kDeviceFreeMarginBytes ? p.device_free_bytes - kDeviceFreeMarginBytes : 0;
		const uint64_t device_max_pages = room / p.page_bytes;
		if (target > device_max_pages) {
			target = device_max_pages;
		}
	}
	return target > p.current_pages ? uint32_t(target) : 0u;
}

// Whether a grow may keep a new allocation. Unknown placement is kept (the backend cannot tell).
inline bool grow_keeps_allocation(bool p_placement_known, bool p_device_local) {
	return !p_placement_known || p_device_local;
}

} // namespace gs_streaming_pool_growth

#endif // GS_STREAMING_POOL_GROWTH_POLICY_H
