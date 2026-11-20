#ifndef GS_HLOD_PAGE_POOL_H
#define GS_HLOD_PAGE_POOL_H

#include "../core/gs_vector_alloc.h"
#include "core/templates/local_vector.h"

namespace gs_hlod_runtime {

// Owner-thread HLOD pages and table slots. A slot stays owned for the payload's entire GPU lifetime.
class HlodPagePool {
public:
	static constexpr uint32_t FREE = UINT32_MAX;

	void clear() {
		free_pages.clear(); free_positions.clear(); free_slots.clear(); forward.clear(); owners.clear();
		released_pages.clear(); released_slots.clear();
		page_release_queued.clear(); slot_release_queued.clear();
		free_count = slot_count = slot_width = 0;
#ifdef TESTS_ENABLED
		require_contiguous = true; // MUTATION (#1514 evidence, never merged): the pool requires contiguity again.
#endif
	}
	bool reset(uint32_t p_capacity, uint32_t p_slot_width) {
		clear();
		if (p_capacity == 0 || p_slot_width == 0 || uint64_t(p_capacity) * p_slot_width >= UINT32_MAX ||
				!gs_resize_or_fail(free_pages, p_capacity, "HLOD free pages") ||
				!gs_resize_or_fail(free_positions, p_capacity, "HLOD free page positions") ||
				!gs_resize_or_fail(free_slots, p_capacity, "HLOD free table slots") ||
				!gs_resize_or_fail(page_release_queued, p_capacity, "HLOD released page marks") ||
				!gs_resize_or_fail(slot_release_queued, p_capacity, "HLOD released slot marks") ||
				!gs_resize_or_fail(forward, uint64_t(p_capacity) * p_slot_width, "HLOD forward page table") ||
				!gs_resize_or_fail(owners, p_capacity, "HLOD reverse page table")) {
			clear();
			return false;
		}
		slot_width = p_slot_width;
		free_count = slot_count = p_capacity;
		forward.fill(FREE);
		owners.fill(FREE);
		page_release_queued.fill(0);
		slot_release_queued.fill(0);
		released_pages.reserve(p_capacity);
		released_slots.reserve(p_capacity);
		for (uint32_t i = 0; i < p_capacity; ++i) {
			free_pages.write[i] = free_slots.write[i] = p_capacity - i - 1;
			free_positions.write[p_capacity - i - 1] = i;
		}
		return true;
	}
	bool allocate(uint32_t p_node, uint32_t p_count, Vector<uint32_t> &r_pages, uint32_t &r_base) {
		if (p_count == 0 || p_count > slot_width || p_count > free_count || slot_count == 0) { return false; }
#ifdef TESTS_ENABLED
		uint32_t contiguous_first = FREE;
		if (require_contiguous) {
			uint32_t run = 0;
			for (uint32_t p = 0; p < uint32_t(owners.size()); ++p) {
				run = owners[p] == FREE ? run + 1 : 0;
				if (run == p_count) { contiguous_first = p + 1 - p_count; break; }
			}
			if (contiguous_first == FREE) { return false; }
		}
#endif
		if (!gs_resize_or_fail(r_pages, p_count, "HLOD allocation page list")) { return false; }
		r_base = free_slots[--slot_count] * slot_width;
		for (uint32_t i = 0; i < p_count; ++i) {
			uint32_t page = free_pages[free_count - 1];
#ifdef TESTS_ENABLED
			if (require_contiguous) { page = contiguous_first + i; }
#endif
			const uint32_t position = free_positions[page];
			const uint32_t last = free_pages[--free_count];
			free_pages.write[position] = last;
			free_positions.write[last] = position;
			r_pages.write[i] = page;
			forward.write[r_base + i] = page;
			owners.write[page] = p_node;
		}
		return true;
	}
	void release(const Vector<uint32_t> &p_pages, uint32_t p_base) {
		for (uint32_t i = p_pages.size(); i-- > 0;) {
			const uint32_t page = p_pages[i];
			owners.write[page] = FREE;
			forward.write[p_base + i] = FREE;
			free_positions.write[page] = free_count;
			free_pages.write[free_count++] = page;
			if (!page_release_queued[page]) {
				page_release_queued.write[page] = 1;
				released_pages.push_back(page);
			}
		}
		const uint32_t slot = p_base / slot_width;
		free_slots.write[slot_count++] = slot;
		if (!slot_release_queued[slot]) {
			slot_release_queued.write[slot] = 1;
			released_slots.push_back(p_base);
		}
	}
	bool matches(uint32_t p_node, const Vector<uint32_t> &p_pages, uint32_t p_base) const {
		if (slot_width == 0 || p_pages.is_empty() || uint64_t(p_pages.size()) > slot_width || p_base % slot_width != 0 ||
				uint64_t(p_base) + p_pages.size() > uint64_t(forward.size())) { return false; }
		for (uint32_t i = 0; i < uint32_t(p_pages.size()); ++i) {
			const uint32_t page = p_pages[i];
			if (page >= uint32_t(owners.size()) || forward[p_base + i] != page || owners[page] != p_node) { return false; }
		}
		return true;
	}
	uint32_t get_free_page_count() const { return free_count; }
	uint32_t max_pages_per_node() const { return slot_width; }
	const Vector<uint32_t> &page_table() const { return forward; }
	const Vector<uint32_t> &page_owners() const { return owners; }
	void take_released_pages(LocalVector<uint32_t> &r_pages) {
		r_pages = released_pages;
		for (uint32_t page : released_pages) { page_release_queued.write[page] = 0; }
		released_pages.clear();
	}
	void take_released_slots(LocalVector<uint32_t> &r_bases) {
		r_bases = released_slots;
		for (uint32_t base : released_slots) { slot_release_queued.write[base / slot_width] = 0; }
		released_slots.clear();
	}
#ifdef TESTS_ENABLED
	void test_set_require_contiguous(bool p_enabled) { require_contiguous = p_enabled; }
	void test_reverse_free_pages() {
		// Alternate opposite ends, so a multi-page node receives separated physical pages.
		Vector<uint32_t> reordered;
		if (!gs_resize_or_fail(reordered, free_count, "HLOD test page permutation")) { return; }
		for (uint32_t i = 0; i < free_count; ++i) {
			const uint32_t source = (i & 1u) ? free_count - 1 - i / 2 : i / 2;
			reordered.write[free_count - 1u - i] = free_pages[source];
		}
		for (uint32_t i = 0; i < free_count; ++i) { free_pages.write[i] = reordered[i]; }
		for (uint32_t i = 0; i < free_count; ++i) { free_positions.write[free_pages[i]] = i; }
	}
#endif

private:
	Vector<uint32_t> free_pages, free_positions, free_slots;
	Vector<uint32_t> forward, owners;
	Vector<uint8_t> page_release_queued, slot_release_queued;
	LocalVector<uint32_t> released_pages, released_slots;
	uint32_t free_count = 0, slot_count = 0, slot_width = 0;
#ifdef TESTS_ENABLED
	bool require_contiguous = false;
#endif
};

} // namespace gs_hlod_runtime

#endif
