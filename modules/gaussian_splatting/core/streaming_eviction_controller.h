#ifndef STREAMING_EVICTION_CONTROLLER_H
#define STREAMING_EVICTION_CONTROLLER_H

#include "core/templates/local_vector.h"
#include <cstdint>

class GaussianStreamingSystem;

class StreamingEvictionController {
public:
    enum class EvictionResult {
        NoEviction,
        EvictedNonVisible,
        EvictedVisible,
        SkippedAllVisible,
    };

    struct NonPrimaryEvictionCandidate {
        uint32_t asset_id = UINT32_MAX;
        uint32_t chunk_id = UINT32_MAX;
        uint64_t last_used_frame = UINT64_MAX;
        float distance = 0.0f;
    };

    void load_streaming_tuning_config_from_project_settings();
    void reset_per_frame_counters();
    void touch_chunk_use(uint64_t &r_last_used_frame);
    void invalidate_candidate_cache();
    void invalidate_resident_tracking();
    void note_chunk_loaded(uint32_t p_asset_id, uint32_t p_chunk_id);
    void note_chunk_unloaded(uint32_t p_asset_id, uint32_t p_chunk_id);
    void record_eviction_result(EvictionResult p_result);
    void record_total_eviction();

    uint32_t get_max_evictions_per_frame() const { return max_evictions_per_frame; }
    uint32_t get_chunks_evicted_this_frame() const { return chunks_evicted_this_frame; }
    uint32_t get_visible_chunks_evicted_this_frame() const { return visible_chunks_evicted_this_frame; }

    // #1088: p_visible_fit_pages > 0 restricts a VISIBLE victim to one whose release completes a
    // contiguous free run of that many atlas pages, so an on-screen chunk is only ever evicted when
    // that eviction alone lets the incoming chunk load. Non-visible victims stay plain LRU.
    EvictionResult evict_least_recently_used(GaussianStreamingSystem &system, bool p_allow_visible_eviction,
            uint32_t p_visible_fit_pages = 0);
    // Returns EvictionResult and does NOT internally call record_eviction_result(),
    // matching the contract of evict_least_recently_used() so callers can drive
    // bookkeeping explicitly. Returns SkippedAllVisible when the admission intent
    // protects all remaining candidates; ordinary budget eviction allows visibility.
    EvictionResult evict_non_primary_lru(GaussianStreamingSystem &system, bool p_allow_visible_eviction = true);

private:
    static uint64_t make_chunk_key(uint32_t p_asset_id, uint32_t p_chunk_id);
    void ensure_resident_tracking(GaussianStreamingSystem &system);

    uint64_t chunk_load_counter = 0;
    uint32_t eviction_hysteresis_frames = 5;
    uint32_t max_evictions_per_frame = 4;
    uint32_t chunks_evicted_this_frame = 0;
    uint32_t visible_chunks_evicted_this_frame = 0;
    uint64_t last_stabilize_log_frame = 0;
    uint64_t cached_eviction_frame = UINT64_MAX;
    uint64_t cached_non_primary_lru_frame = UINT64_MAX;
    uint32_t cached_non_primary_lru_cursor = 0;
    bool resident_tracking_dirty = true;
    LocalVector<uint32_t> loaded_primary_chunks;
    LocalVector<uint64_t> loaded_non_primary_chunk_keys;
    LocalVector<NonPrimaryEvictionCandidate> cached_non_primary_lru_candidates;
    LocalVector<uint32_t> cached_visible_chunks;
    LocalVector<uint32_t> cached_nonvisible_chunks;
};

#endif // STREAMING_EVICTION_CONTROLLER_H
