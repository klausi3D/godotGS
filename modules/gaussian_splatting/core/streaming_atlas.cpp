/**************************************************************************/
/* streaming_atlas.cpp                                                    */
/*                                                                        */
/* GaussianAtlasAllocator method implementations and                      */
/* GaussianStreamingSystem residency helpers.                             */
/**************************************************************************/

#include "streaming_atlas.h"

#include "gaussian_streaming.h"
#include "../logger/gs_debug_trace.h"
#include <cstdint>

void GaussianAtlasAllocator::reset(uint32_t p_page_count) {
	capacity = p_page_count;
	free_page_count = p_page_count;
	free_runs.clear();
	run_map.clear();
	if (capacity > 0) {
		free_runs.push_back(PageRun{ 0u, capacity });
	}
}

bool GaussianAtlasAllocator::resize_preserve(uint32_t p_new_capacity) {
	if (p_new_capacity <= capacity) {
		return false;
	}
	const uint32_t old_capacity = capacity;
	capacity = p_new_capacity;
	_insert_free_run(old_capacity, p_new_capacity - old_capacity);
	return true;
}

bool GaussianAtlasAllocator::can_allocate(uint32_t p_page_count) const {
	if (p_page_count == 0) {
		return false;
	}
	for (const PageRun &run : free_runs) {
		if (run.page_count >= p_page_count) {
			return true;
		}
	}
	return false;
}

bool GaussianAtlasAllocator::can_allocate_all(const uint32_t *p_pages, uint32_t p_count, uint32_t *r_first_failing_pages, uint64_t *r_placed_pages) const {
	LocalVector<PageRun> simulated_free_runs = free_runs;
	return simulate_allocate_all(simulated_free_runs, p_pages, p_count, r_first_failing_pages, r_placed_pages);
}

// Preserve request order, matching repeated allocate_slot() calls. Best-fit and its
// lowest-address tie match allocate_slot() below (streaming_atlas.cpp:109-118).
bool GaussianAtlasAllocator::simulate_allocate_all(LocalVector<PageRun> &r_free_runs, const uint32_t *p_pages, uint32_t p_count, uint32_t *r_first_failing_pages, uint64_t *r_placed_pages) {
	if (r_first_failing_pages) {
		*r_first_failing_pages = 0;
	}
	if (r_placed_pages) {
		*r_placed_pages = 0;
	}
	for (uint32_t request = 0; request < p_count; ++request) {
		const uint32_t pages = p_pages[request];
		uint32_t best = UINT32_MAX;
		for (uint32_t i = 0; pages > 0 && i < r_free_runs.size(); ++i) {
			const PageRun &hole = r_free_runs[i];
			if (hole.page_count >= pages && (best == UINT32_MAX || hole.page_count < r_free_runs[best].page_count ||
					(hole.page_count == r_free_runs[best].page_count && hole.first_page < r_free_runs[best].first_page))) {
				best = i;
			}
		}
		if (best == UINT32_MAX) {
			if (r_first_failing_pages) {
				*r_first_failing_pages = pages;
			}
			return false;
		}
		PageRun &hole = r_free_runs[best];
		if (hole.page_count == pages) {
			r_free_runs.remove_at(best);
		} else {
			hole.first_page += pages;
			hole.page_count -= pages;
		}
		if (r_placed_pages) {
			*r_placed_pages += pages;
		}
	}
	return true;
}

uint32_t GaussianAtlasAllocator::get_largest_free_run() const {
	uint32_t largest = 0;
	for (const PageRun &run : free_runs) {
		largest = MAX(largest, run.page_count);
	}
	return largest;
}

bool GaussianAtlasAllocator::allocate_slot(uint64_t p_chunk_key, uint32_t p_page_count, uint32_t &r_slot) {
	if (const PageRun *existing = run_map.getptr(p_chunk_key)) {
		if (existing->page_count != p_page_count) {
			return false;
		}
		r_slot = existing->first_page;
		return true;
	}
	if (p_page_count == 0) {
		return false;
	}
	// Best fit: the smallest free run that holds the request; the lowest address wins a tie
	// because free_runs is sorted by first_page and only a strictly smaller run replaces it.
	uint32_t best = UINT32_MAX;
	for (uint32_t i = 0; i < free_runs.size(); i++) {
		const uint32_t len = free_runs[i].page_count;
		if (len >= p_page_count && (best == UINT32_MAX || len < free_runs[best].page_count)) {
			best = i;
			if (len == p_page_count) {
				break;
			}
		}
	}
	if (best == UINT32_MAX) {
		return false;
	}
	PageRun &hole = free_runs[best];
	const PageRun run{ hole.first_page, p_page_count };
	if (hole.page_count == p_page_count) {
		free_runs.remove_at(best);
	} else {
		hole.first_page += p_page_count;
		hole.page_count -= p_page_count;
	}
	free_page_count -= p_page_count;
	run_map[p_chunk_key] = run;
	r_slot = run.first_page;
	return true;
}

bool GaussianAtlasAllocator::get_free_run(uint32_t p_index, PageRun &r_run) const {
	if (p_index >= free_runs.size()) { return false; }
	r_run = free_runs[p_index];
	return true;
}

bool GaussianAtlasAllocator::allocate_run_at(uint64_t p_chunk_key, const PageRun &p_run) {
	if (p_run.page_count == 0 || p_run.first_page > capacity || p_run.page_count > capacity - p_run.first_page) { return false; }
	if (const PageRun *existing = run_map.getptr(p_chunk_key)) {
		return existing->first_page == p_run.first_page && existing->page_count == p_run.page_count;
	}
	for (uint32_t i = 0; i < free_runs.size(); ++i) {
		const PageRun hole = free_runs[i];
		if (p_run.first_page < hole.first_page || p_run.first_page - hole.first_page > hole.page_count ||
				p_run.page_count > hole.page_count - (p_run.first_page - hole.first_page)) { continue; }
		const uint32_t before = p_run.first_page - hole.first_page;
		const uint32_t after = hole.page_count - before - p_run.page_count;
		if (before != 0) {
			free_runs[i].page_count = before;
			if (after != 0) { free_runs.insert(i + 1, PageRun{ p_run.first_page + p_run.page_count, after }); }
		} else if (after != 0) {
			free_runs[i] = PageRun{ p_run.first_page + p_run.page_count, after };
		} else {
			free_runs.remove_at(i);
		}
		free_page_count -= p_run.page_count;
		run_map[p_chunk_key] = p_run;
		return true;
	}
	return false;
}

void GaussianAtlasAllocator::_insert_free_run(uint32_t p_first_page, uint32_t p_page_count) {
	if (p_page_count == 0) {
		return;
	}
	free_page_count += p_page_count;
	// Find the first free run that starts after the inserted one.
	uint32_t idx = 0;
	while (idx < free_runs.size() && free_runs[idx].first_page < p_first_page) {
		idx++;
	}
	const bool merge_prev = idx > 0 &&
			free_runs[idx - 1].first_page + free_runs[idx - 1].page_count == p_first_page;
	const bool merge_next = idx < free_runs.size() &&
			p_first_page + p_page_count == free_runs[idx].first_page;
	if (merge_prev && merge_next) {
		free_runs[idx - 1].page_count += p_page_count + free_runs[idx].page_count;
		free_runs.remove_at(idx);
	} else if (merge_prev) {
		free_runs[idx - 1].page_count += p_page_count;
	} else if (merge_next) {
		free_runs[idx].first_page = p_first_page;
		free_runs[idx].page_count += p_page_count;
	} else {
		free_runs.insert(idx, PageRun{ p_first_page, p_page_count });
	}
}

void GaussianAtlasAllocator::release_slot(uint64_t p_chunk_key) {
	const PageRun *run = run_map.getptr(p_chunk_key);
	if (!run) {
		return;
	}
	const PageRun released = *run;
	run_map.erase(p_chunk_key);
	_insert_free_run(released.first_page, released.page_count);
}

bool GaussianAtlasAllocator::get_slot(uint64_t p_chunk_key, uint32_t &r_slot) const {
	if (const PageRun *run = run_map.getptr(p_chunk_key)) {
		r_slot = run->first_page;
		return true;
	}
	return false;
}

bool GaussianAtlasAllocator::get_run(uint64_t p_chunk_key, PageRun &r_run) const {
	if (const PageRun *run = run_map.getptr(p_chunk_key)) {
		r_run = *run;
		return true;
	}
	return false;
}

uint32_t GaussianAtlasAllocator::get_coalesced_run_if_released(const PageRun &p_run) const {
	uint32_t first = p_run.first_page;
	uint32_t end = p_run.first_page + p_run.page_count;
	for (const PageRun &free_run : free_runs) {
		if (free_run.first_page + free_run.page_count == p_run.first_page) {
			first = free_run.first_page;
		} else if (free_run.first_page == end) {
			end = free_run.first_page + free_run.page_count;
		}
	}
	return end - first;
}

void GaussianAtlasAllocator::clear() {
	capacity = 0;
	free_page_count = 0;
	free_runs.clear();
	run_map.clear();
}

void GaussianStreamingSystem::_apply_requested_residency(bool can_async_pack) {
	if (!asset_registry.request_pending) {
		return;
	}
	const bool trace_enabled = GaussianSplatting::debug_trace_is_enabled();
	if (trace_enabled) {
		GaussianSplatting::debug_trace_record_event("streaming",
				vformat("ApplyResidency START: atlas_asset_order=%d", asset_registry.atlas_asset_order.size()),
				false);
	}

    bool has_deferred_requested_chunks = false;
    for (uint32_t asset_id : asset_registry.atlas_asset_order) {
        AtlasAssetState *asset = _get_asset_state(asset_id);
        if (!asset || !asset->data.is_valid()) {
            if (trace_enabled) {
                GaussianSplatting::debug_trace_record_event("streaming",
						vformat("ApplyResidency SKIP INVALID asset_id=%d", asset_id),
						true);
			}
			continue;
		}

        LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
        if (trace_enabled) {
            GaussianSplatting::debug_trace_record_event("streaming",
					vformat("ApplyResidency asset_id=%d chunks=%d requested_chunks=%d",
					asset_id, asset_chunks.size(), asset->requested_chunks.size()),
				false);
        }
        if (asset_id != PRIMARY_ASSET_ID) {
            _evict_unrequested_chunks(asset_id, *asset, asset_chunks);
        }
        const bool deferred_chunks = _load_requested_chunks(asset_id, *asset, asset_chunks, trace_enabled, can_async_pack);
        has_deferred_requested_chunks = has_deferred_requested_chunks || deferred_chunks;
    }

	if (trace_enabled) {
		GaussianSplatting::debug_trace_record_event("streaming",
				vformat("ApplyResidency END: loaded_chunks_count=%d vram=%s", budget.loaded_chunks_count, String::num_uint64(_get_total_vram_usage_bytes())),
				false);
	}
	asset_registry.request_pending = has_deferred_requested_chunks;
	asset_registry.request_collection_active = false;
}

void GaussianStreamingSystem::_evict_unrequested_chunks(uint32_t asset_id, AtlasAssetState &asset,
		LocalVector<StreamingChunk> &asset_chunks) {
	for (uint32_t i = 0; i < asset_chunks.size(); i++) {
		StreamingChunk &chunk = asset_chunks[i];
		const bool requested = _is_requested_chunk_in_current_generation(asset, i);
		if (requested) {
			continue;
		}

		if (chunk.upload_pending) {
			upload_pipeline.cancel_chunk_jobs(*this, asset_id, i, chunk.buffer_slot);
		}
		if (chunk.is_loaded) {
			_unload_chunk(asset_id, i);
			eviction_controller.record_total_eviction();
		}
	}
}

bool GaussianStreamingSystem::_load_requested_chunks(uint32_t asset_id, AtlasAssetState &asset,
		LocalVector<StreamingChunk> &asset_chunks, bool trace_enabled, bool can_async_pack) {
	int chunks_processed = 0;
	int chunks_already_loaded = 0;
	int chunks_queued = 0;
	bool has_deferred_chunks = false;

	for (uint32_t chunk_id : asset.requested_chunks) {
		if (chunk_id >= asset_chunks.size()) {
			continue;
		}
		chunks_processed++;
        const GaussianStreamingSystem::RequestedChunkState *request_state =
                asset.requested_chunk_state.getptr(chunk_id);
        if (request_state &&
                request_state->stamp == asset_registry.request_generation &&
                request_state->request_result == GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED) {
            continue;
        }
        StreamingChunk &chunk = asset_chunks[chunk_id];
        if (chunk.is_loaded || chunk.upload_pending) {
            chunks_already_loaded++;
            if (chunk.is_loaded) {
                chunk.explicit_request_generation = 0;
                eviction_controller.touch_chunk_use(chunk.last_used_frame, total_frame_count);
                _update_requested_chunk_state(asset, chunk_id,
                        GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_SATISFIED,
                        GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_SATISFIED);
            } else {
                chunk.explicit_request_generation = request_state ? request_state->stamp : asset_registry.request_generation;
                _update_requested_chunk_state(asset, chunk_id,
                        GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_QUEUED,
                        GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_QUEUED);
            }
            continue;
        }
        const bool queued = _enqueue_chunk_load_request(asset_id, chunk_id, can_async_pack, !can_async_pack);

        if (queued) {
            chunks_queued++;
            chunk.explicit_request_generation = request_state ? request_state->stamp : asset_registry.request_generation;
            _update_requested_chunk_state(asset, chunk_id,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_QUEUED,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_QUEUED);
            if (!can_async_pack) {
                has_deferred_chunks = true;
            }
        }
        if (!queued && !chunk.is_loaded && !chunk.upload_pending) {
            if (trace_enabled) {
                GaussianSplatting::debug_trace_record_event("streaming",
                        vformat("ApplyResidency DEFER chunk_id=%d", chunk_id),
                        true);
            }
            _update_requested_chunk_state(asset, chunk_id,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_DEFERRED,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_DEFERRED,
                    ERR_BUSY);
            has_deferred_chunks = true;
        }
        if (!can_async_pack && !chunk.is_loaded && !chunk.upload_pending) {
            has_deferred_chunks = true;
        }
	}

	if (trace_enabled) {
		GaussianSplatting::debug_trace_record_event("streaming",
				vformat("ApplyResidency asset_id=%d processed=%d already_loaded=%d queued=%d",
						asset_id, chunks_processed, chunks_already_loaded, chunks_queued),
				false);
	}
	return has_deferred_chunks;
}
