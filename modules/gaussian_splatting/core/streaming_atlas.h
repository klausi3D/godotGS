#ifndef STREAMING_ATLAS_H
#define STREAMING_ATLAS_H

/**************************************************************************/
/* streaming_atlas.h                                                      */
/*                                                                        */
/* Atlas slot allocator for the streaming system.                         */
/*                                                                        */
/* Pattern 10 (Flyweight + GPU resource cache): The atlas allocator maps   */
/* many streaming chunks to a fixed pool of GPU atlas slots. Each slot is  */
/* a flyweight reference into a single, shared GPU buffer.                 */
/*                                                                        */
/* Pattern 2 (Strong types): Slot indices are uint32_t, chunk keys are    */
/* uint64_t (asset_id << 32 | chunk_idx encoding).                        */
/**************************************************************************/

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include <cstdint>

// #1088: the atlas is allocated in fixed-size PAGES, and every chunk owns one
// contiguous run of pages sized to its own splat count. A chunk's "slot" id is the
// first page of its run, so the GPU-side splat base is slot * page_splats and the
// shaders (which index gaussians[atlas_base + i]) are unchanged. Before #1088 every
// chunk took a fixed 65,536-splat slot regardless of its size.
//
// Placement is best-fit (smallest free run that fits, lowest address on a tie) and
// freed runs coalesce with their neighbours immediately. There is no compaction: when
// no run fits, admission evicts least-recently-used chunks until one does (bounded by
// the per-frame eviction budget; see GaussianStreamingSystem::_evict_until_atlas_fit).
class GaussianAtlasAllocator {
public:
	struct PageRun {
		uint32_t first_page = 0;
		uint32_t page_count = 0;
	};

	void reset(uint32_t p_page_count);
	// Grow capacity (in pages) while preserving existing runs. Returns false if
	// `p_new_capacity` is not strictly greater than the current capacity
	// (shrinking is intentionally unsupported).
	bool resize_preserve(uint32_t p_new_capacity);
	// True when a contiguous free run of at least `p_page_count` pages exists.
	bool can_allocate(uint32_t p_page_count) const;
	// Dry-run sequential best-fit placement in the caller's supplied member order.
	// Neither allocator state nor p_pages is changed. Outputs describe the placed
	// prefix and the first failed request (zero when every request fits).
	bool can_allocate_all(const uint32_t *p_pages, uint32_t p_count, uint32_t *r_first_failing_pages = nullptr, uint64_t *r_placed_pages = nullptr) const;
	// Reusable planner seam: p_pages is consumed in its supplied order. Only the
	// supplied scratch free runs are consumed; allocator state is never accessed.
	static bool simulate_allocate_all(LocalVector<PageRun> &r_free_runs, const uint32_t *p_pages, uint32_t p_count, uint32_t *r_first_failing_pages = nullptr, uint64_t *r_placed_pages = nullptr);
	uint32_t get_free_page_count() const { return free_page_count; }
	uint32_t get_used_page_count() const { return capacity - free_page_count; }
	uint32_t get_largest_free_run() const;
	uint32_t get_free_run_count() const { return free_runs.size(); }
	uint32_t get_allocation_count() const { return run_map.size(); }
	uint32_t get_capacity() const { return capacity; }
	// Allocate a contiguous run of `p_page_count` pages for `p_chunk_key`; `r_slot`
	// receives the run's first page. Re-allocating a key that already owns a run
	// returns that run only if it has the requested size.
	bool allocate_slot(uint64_t p_chunk_key, uint32_t p_page_count, uint32_t &r_slot);
	// Owner-thread inspection/reservation for HLOD relocation. Exact reservation never relocates an owner.
	bool get_free_run(uint32_t p_index, PageRun &r_run) const;
	bool allocate_run_at(uint64_t p_chunk_key, const PageRun &p_run);
	void release_slot(uint64_t p_chunk_key);
	bool get_slot(uint64_t p_chunk_key, uint32_t &r_slot) const;
	bool get_run(uint64_t p_chunk_key, PageRun &r_run) const;
	// Length of the free run that releasing `p_run` would leave (the run plus any free
	// neighbours it coalesces with).
	uint32_t get_coalesced_run_if_released(const PageRun &p_run) const;
	void clear();

private:
	void _insert_free_run(uint32_t p_first_page, uint32_t p_page_count);

	uint32_t capacity = 0;
	uint32_t free_page_count = 0;
	// Free runs, sorted by first_page, never adjacent (always coalesced).
	LocalVector<PageRun> free_runs;
	HashMap<uint64_t, PageRun> run_map;
};

#endif // STREAMING_ATLAS_H
