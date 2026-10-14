#ifndef STREAMING_GLOBAL_ATLAS_REGISTRY_H
#define STREAMING_GLOBAL_ATLAS_REGISTRY_H

#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "../renderer/gaussian_gpu_layout.h"
#include <cstdint>

class GaussianStreamingSystem;
class RenderingDevice;

struct GlobalAtlasState {
	RID atlas_gaussian_buffer;
	uint32_t atlas_gaussian_count = 0;
	RID asset_meta_buffer;
	RID chunk_meta_buffer;
	RID asset_chunk_index_buffer;
	RID quantization_buffer;
	uint64_t atlas_generation = 0;
};

class StreamingGlobalAtlasRegistry {
	friend class GaussianStreamingSystem;

public:
	struct ChunkMetaUploadPlan {
		bool full_update = false;
		uint32_t dirty_count = 0;
		uint32_t contiguous_range_count = 0;
	};
	struct SyncDiagnostics {
		uint32_t topology_scan_asset_count = 0;
		uint32_t topology_scan_chunk_count = 0;
		uint32_t cached_total_chunks = 0;
		uint32_t chunk_meta_dirty_count = 0;
		uint32_t chunk_meta_range_count = 0;
		bool used_cached_topology = false;
		bool forced_full_rebuild = false;
		bool chunk_meta_full_update = false;
	};

	void cleanup(RenderingDevice *p_rd);
	void mark_asset_registry_dirty() { asset_registry_dirty = true; }
	void build_cpu_state(GaussianStreamingSystem &system);
	void update_chunk_meta_entry(GaussianStreamingSystem &system, uint32_t asset_id, uint32_t chunk_idx);
	void mark_chunk_meta_dirty(GaussianStreamingSystem &system, uint32_t chunk_idx);
	void mark_chunk_meta_dirty(GaussianStreamingSystem &system, uint32_t asset_id, uint32_t chunk_idx);
	void sync_to_gpu(GaussianStreamingSystem &system, RenderingDevice *p_rd);

	uint32_t get_max_chunk_count_per_asset() const { return max_chunk_count_per_asset; }
	uint32_t get_max_chunk_splats() const { return max_chunk_splats; }
	uint32_t get_atlas_published_chunks() const { return atlas_published_chunk_count; }
	uint64_t get_auxiliary_vram_overhead_bytes() const;
	const GlobalAtlasState &get_global_atlas_state() const { return global_atlas_state; }
	uint64_t get_atlas_generation() const { return global_atlas_state.atlas_generation; }
	const SyncDiagnostics &get_last_sync_diagnostics() const { return last_sync_diagnostics; }
#if defined(TESTS_ENABLED)
	static ChunkMetaUploadPlan _test_plan_chunk_meta_uploads(
			const LocalVector<uint32_t> &p_dirty_indices, uint32_t p_total_chunks);
	ChunkMetaUploadPlan _test_plan_chunk_meta_sync(GaussianStreamingSystem &system);
	void _test_clear_cpu_dirty_state();
#endif

private:
	struct SyncPreparation {
		bool atlas_dirty = false;
		bool rebuild_cpu_state = false;
		uint32_t dense_count = 0;
		uint32_t total_chunks = 0;
	};

	void _invalidate_chunk_meta_tracking();
	void _invalidate_published_buffers();
	void _reset_sync_diagnostics();
	bool _has_stable_cached_topology(uint32_t p_dense_count) const;
	uint32_t _scan_total_chunks_for_sync(GaussianStreamingSystem &system);
	SyncPreparation _prepare_sync_state(GaussianStreamingSystem &system, bool p_quantization_rebuild);
	static ChunkMetaUploadPlan _plan_chunk_meta_uploads(
			LocalVector<uint32_t> &r_dirty_indices, uint32_t p_total_chunks);

	GlobalAtlasState global_atlas_state;
	uint64_t atlas_generation_counter = 0;
	SyncDiagnostics last_sync_diagnostics;
	uint32_t max_chunk_count_per_asset = 0;
	uint32_t max_chunk_splats = 0;
	uint32_t atlas_published_chunk_count = 0;
	uint32_t cached_total_chunks = 0;
	uint32_t cached_dense_count = 0;
	RID asset_meta_buffer;
	RID chunk_meta_buffer;
	RID asset_chunk_index_buffer;
	uint32_t asset_meta_buffer_size = 0;
	uint32_t chunk_meta_buffer_size = 0;
	uint32_t asset_chunk_index_buffer_size = 0;
	LocalVector<AssetMetaGPU> asset_meta_cpu;
	LocalVector<ChunkMetaGPU> chunk_meta_cpu;
	LocalVector<AssetChunkIndexGPU> asset_chunk_index_cpu;
	LocalVector<uint32_t> chunk_meta_dirty_indices;
	LocalVector<uint8_t> chunk_meta_dirty_flags;
	bool asset_meta_dirty = false;
	bool asset_chunk_index_dirty = false;
	bool chunk_meta_dirty_all = false;
	bool asset_registry_dirty = false;
	bool cpu_state_valid = false;
};

#endif // STREAMING_GLOBAL_ATLAS_REGISTRY_H
