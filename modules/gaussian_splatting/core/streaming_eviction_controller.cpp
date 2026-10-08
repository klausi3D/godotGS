#include "streaming_eviction_controller.h"

#include "gaussian_streaming.h"
#include "gs_project_settings.h"
#include "core/config/project_settings.h"
#include "../logger/gs_logger.h"

#include <algorithm>

void StreamingEvictionController::load_streaming_tuning_config_from_project_settings() {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return;
    }

    eviction_hysteresis_frames = gs::settings::get_uint(ps,
            "rendering/gaussian_splatting/streaming/eviction_hysteresis_frames",
            eviction_hysteresis_frames);
    max_evictions_per_frame = gs::settings::get_uint(ps,
            "rendering/gaussian_splatting/streaming/max_evictions_per_frame",
            max_evictions_per_frame);
    invalidate_candidate_cache();
}

void StreamingEvictionController::reset_per_frame_counters() {
    chunks_evicted_this_frame = 0;
    visible_chunks_evicted_this_frame = 0;
}

void StreamingEvictionController::touch_chunk_use(uint64_t &r_last_used_frame, uint64_t p_frame_generation) {
    r_last_used_frame = p_frame_generation;
}

void StreamingEvictionController::invalidate_candidate_cache() {
    cached_eviction_frame = UINT64_MAX;
    cached_non_primary_lru_frame = UINT64_MAX;
    cached_non_primary_lru_cursor = 0;
    cached_visible_chunks.clear();
    cached_nonvisible_chunks.clear();
    cached_non_primary_lru_candidates.clear();
}

void StreamingEvictionController::invalidate_resident_tracking() {
    loaded_primary_chunks.clear();
    loaded_non_primary_chunk_keys.clear();
    resident_tracking_dirty = true;
    invalidate_candidate_cache();
}

uint64_t StreamingEvictionController::make_chunk_key(uint32_t p_asset_id, uint32_t p_chunk_id) {
    return (uint64_t(p_asset_id) << 32) | uint64_t(p_chunk_id);
}

void StreamingEvictionController::note_chunk_loaded(uint32_t p_asset_id, uint32_t p_chunk_id) {
    if (p_asset_id == GaussianStreamingSystem::PRIMARY_ASSET_ID) {
        if (!loaded_primary_chunks.has(p_chunk_id)) {
            loaded_primary_chunks.ordered_insert(p_chunk_id);
        }
    } else {
        const uint64_t chunk_key = make_chunk_key(p_asset_id, p_chunk_id);
        if (!loaded_non_primary_chunk_keys.has(chunk_key)) {
            loaded_non_primary_chunk_keys.ordered_insert(chunk_key);
        }
    }
    invalidate_candidate_cache();
}

void StreamingEvictionController::note_chunk_unloaded(uint32_t p_asset_id, uint32_t p_chunk_id) {
    if (p_asset_id == GaussianStreamingSystem::PRIMARY_ASSET_ID) {
        loaded_primary_chunks.erase(p_chunk_id);
    } else {
        loaded_non_primary_chunk_keys.erase(make_chunk_key(p_asset_id, p_chunk_id));
    }
    invalidate_candidate_cache();
}

void StreamingEvictionController::ensure_resident_tracking(GaussianStreamingSystem &system) {
    if (!resident_tracking_dirty) {
        return;
    }

    loaded_primary_chunks.clear();
    loaded_non_primary_chunk_keys.clear();

    for (uint32_t chunk_id = 0; chunk_id < system.chunks.size(); chunk_id++) {
        if (system.chunks[chunk_id].is_loaded) {
            loaded_primary_chunks.push_back(chunk_id);
        }
    }

    for (uint32_t asset_id : system.asset_registry.atlas_asset_order) {
        if (asset_id == GaussianStreamingSystem::PRIMARY_ASSET_ID) {
            continue;
        }
        GaussianStreamingSystem::AtlasAssetState *asset = system._get_asset_state(asset_id);
        if (!asset) {
            continue;
        }
        LocalVector<GaussianStreamingSystem::StreamingChunk> &asset_chunks = system._get_asset_chunks(*asset);
        for (uint32_t chunk_id = 0; chunk_id < asset_chunks.size(); chunk_id++) {
            if (asset_chunks[chunk_id].is_loaded) {
                loaded_non_primary_chunk_keys.push_back(make_chunk_key(asset_id, chunk_id));
            }
        }
    }

    resident_tracking_dirty = false;
    invalidate_candidate_cache();
}

void StreamingEvictionController::record_eviction_result(EvictionResult p_result) {
    if (p_result == EvictionResult::EvictedNonVisible || p_result == EvictionResult::EvictedVisible) {
        chunks_evicted_this_frame++;
        if (p_result == EvictionResult::EvictedVisible) {
            visible_chunks_evicted_this_frame++;
        }
    }
}

void StreamingEvictionController::record_total_eviction() {
    chunks_evicted_this_frame++;
}

StreamingEvictionController::EvictionResult StreamingEvictionController::evict_least_recently_used(
        GaussianStreamingSystem &system, bool p_allow_visible_eviction, uint32_t p_visible_fit_pages) {
    ensure_resident_tracking(system);

    if (cached_eviction_frame != system.total_frame_count) {
        cached_visible_chunks.clear();
        cached_nonvisible_chunks.clear();
        uint32_t resident_scan_count = 0;
        uint32_t candidate_count = 0;
        for (uint32_t i : loaded_primary_chunks) {
            resident_scan_count++;
            if (i >= system.chunks.size()) {
                continue;
            }
            const GaussianStreamingSystem::StreamingChunk &chunk = system.chunks[i];
            if (!chunk.is_loaded || chunk.upload_pending) {
                continue;
            }
            if (eviction_hysteresis_frames > 0 &&
                    system.total_frame_count - chunk.last_loaded_frame < eviction_hysteresis_frames) {
                continue;
            }
            if (chunk.is_visible) {
                cached_visible_chunks.push_back(i);
            } else {
                cached_nonvisible_chunks.push_back(i);
            }
            candidate_count++;
        }
        cached_eviction_frame = system.total_frame_count;
        system.scheduler.last_primary_eviction_scan_count = resident_scan_count;
        system.scheduler.last_primary_eviction_candidate_count = candidate_count;
    }

    uint32_t best_nonvis_idx = UINT32_MAX;
    uint64_t best_nonvis_frame = UINT64_MAX;
    float best_nonvis_distance = -1.0f;

    uint32_t best_vis_idx = UINT32_MAX;
    uint64_t best_vis_frame = UINT64_MAX;
    float best_vis_distance = -1.0f;

    for (uint32_t i : cached_nonvisible_chunks) {
        const GaussianStreamingSystem::StreamingChunk &chunk = system.chunks[i];
        if (!chunk.is_loaded || chunk.upload_pending) {
            continue;
        }
        const uint64_t used_frame = chunk.last_used_frame;
        const float dist = chunk.distance;
        if (used_frame < best_nonvis_frame ||
                (used_frame == best_nonvis_frame && dist > best_nonvis_distance)) {
            best_nonvis_frame = used_frame;
            best_nonvis_distance = dist;
            best_nonvis_idx = i;
        }
    }

    for (uint32_t i : cached_visible_chunks) {
        const GaussianStreamingSystem::StreamingChunk &chunk = system.chunks[i];
        if (!chunk.is_loaded || chunk.upload_pending) {
            continue;
        }
        if (p_visible_fit_pages > 0) {
            GaussianAtlasAllocator::PageRun run;
            if (!system.atlas_allocator.get_run(
                        system._make_chunk_key(GaussianStreamingSystem::PRIMARY_ASSET_ID, i), run) ||
                    system.atlas_allocator.get_coalesced_run_if_released(run) < p_visible_fit_pages) {
                continue; // evicting it would pop it on screen without making room
            }
        }
        const uint64_t used_frame = chunk.last_used_frame;
        const float dist = chunk.distance;
        if (used_frame < best_vis_frame ||
                (used_frame == best_vis_frame && dist > best_vis_distance)) {
            best_vis_frame = used_frame;
            best_vis_distance = dist;
            best_vis_idx = i;
        }
    }

    if (best_nonvis_idx != UINT32_MAX) {
        system._unload_chunk(best_nonvis_idx);
        return EvictionResult::EvictedNonVisible;
    }

    if (best_vis_idx != UINT32_MAX) {
        if (!p_allow_visible_eviction) {
            if (system.total_frame_count - last_stabilize_log_frame >= 300) {
                GS_LOG_STREAMING_DEBUG(vformat("[STREAM-STABLE] All %d loaded chunks are visible - stabilizing (not evicting)",
                        system.budget.loaded_chunks_count));
                last_stabilize_log_frame = system.total_frame_count;
            }
            return EvictionResult::SkippedAllVisible;
        }

        system._unload_chunk(best_vis_idx);
        return EvictionResult::EvictedVisible;
    }

    return EvictionResult::NoEviction;
}

StreamingEvictionController::EvictionResult StreamingEvictionController::evict_non_primary_lru(GaussianStreamingSystem &system) {
    // Contract: this returns EvictionResult and does NOT internally call
    // record_eviction_result(). Callers are responsible for recording the
    // result so per-frame eviction-budget bookkeeping stays single-source.
    // (Previously this function returned bool and self-recorded, which made
    // it incompatible with helpers like _evict_for_admission_gate() whose
    // callers also record — producing double-counts.)
    if (max_evictions_per_frame > 0 && chunks_evicted_this_frame >= max_evictions_per_frame) {
        return EvictionResult::NoEviction;
    }

    ensure_resident_tracking(system);

    if (cached_non_primary_lru_frame != system.total_frame_count) {
        cached_non_primary_lru_candidates.clear();
        uint32_t resident_scan_count = 0;
        for (uint64_t chunk_key : loaded_non_primary_chunk_keys) {
            resident_scan_count++;
            const uint32_t asset_id = uint32_t(chunk_key >> 32);
            const uint32_t chunk_id = uint32_t(chunk_key & 0xffffffffu);
            GaussianStreamingSystem::AtlasAssetState *asset = system._get_asset_state(asset_id);
            if (!asset) {
                continue;
            }
            LocalVector<GaussianStreamingSystem::StreamingChunk> &asset_chunks = system._get_asset_chunks(*asset);
            if (chunk_id >= asset_chunks.size()) {
                continue;
            }
            const GaussianStreamingSystem::StreamingChunk &chunk = asset_chunks[chunk_id];
            if (!chunk.is_loaded || chunk.upload_pending) {
                continue;
            }
            // Match the hysteresis filter primary LRU enforces. Without it,
            // a sync-fallback drain that just loaded a non-primary chunk in
            // this same frame can immediately re-evict it as the next
            // admission victim — load-then-evict churn instead of forward
            // progress.
            if (eviction_hysteresis_frames > 0 &&
                    system.total_frame_count - chunk.last_loaded_frame < eviction_hysteresis_frames) {
                continue;
            }
            // Skip explicitly-requested chunks. _unload_chunk() does not
            // downgrade request status, so evicting one would leave the
            // caller observing "requested = satisfied" while the chunk is
            // already gone — breaking requested-residency semantics under
            // multi-asset atlas pressure.
            if (system._is_requested_chunk_in_current_generation(*asset, chunk_id)) {
                continue;
            }
            NonPrimaryEvictionCandidate candidate;
            candidate.asset_id = asset_id;
            candidate.chunk_id = chunk_id;
            candidate.last_used_frame = chunk.last_used_frame;
            candidate.distance = chunk.distance;
            cached_non_primary_lru_candidates.push_back(candidate);
        }
        if (!cached_non_primary_lru_candidates.is_empty()) {
            NonPrimaryEvictionCandidate *candidate_ptr = cached_non_primary_lru_candidates.ptr();
            std::sort(candidate_ptr, candidate_ptr + cached_non_primary_lru_candidates.size(),
                    [](const NonPrimaryEvictionCandidate &a, const NonPrimaryEvictionCandidate &b) {
                        if (a.last_used_frame != b.last_used_frame) {
                            return a.last_used_frame < b.last_used_frame;
                        }
                        if (a.distance != b.distance) {
                            return a.distance > b.distance;
                        }
                        if (a.asset_id != b.asset_id) {
                            return a.asset_id < b.asset_id;
                        }
                        return a.chunk_id < b.chunk_id;
                    });
        }
        cached_non_primary_lru_cursor = 0;
        cached_non_primary_lru_frame = system.total_frame_count;
        system.scheduler.last_non_primary_scan_count = resident_scan_count;
        system.scheduler.last_non_primary_eviction_candidate_count = cached_non_primary_lru_candidates.size();
    }

    while (cached_non_primary_lru_cursor < cached_non_primary_lru_candidates.size()) {
        const NonPrimaryEvictionCandidate &candidate = cached_non_primary_lru_candidates[cached_non_primary_lru_cursor++];
        GaussianStreamingSystem::AtlasAssetState *asset = system._get_asset_state(candidate.asset_id);
        if (!asset) {
            continue;
        }
        LocalVector<GaussianStreamingSystem::StreamingChunk> &asset_chunks = system._get_asset_chunks(*asset);
        if (candidate.chunk_id >= asset_chunks.size()) {
            continue;
        }
        if (!asset_chunks[candidate.chunk_id].is_loaded || asset_chunks[candidate.chunk_id].upload_pending) {
            continue;
        }
        if (system._is_requested_chunk_in_current_generation(*asset, candidate.chunk_id)) {
            continue;
        }

        const bool was_visible = asset_chunks[candidate.chunk_id].is_visible;
        system._unload_chunk(candidate.asset_id, candidate.chunk_id);
        return was_visible ? EvictionResult::EvictedVisible : EvictionResult::EvictedNonVisible;
    }

    return EvictionResult::NoEviction;
}
