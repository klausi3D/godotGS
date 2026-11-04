#ifndef GS_HLOD_BUNDLE_ROWS_H
#define GS_HLOD_BUNDLE_ROWS_H

#include "../core/streaming_quantization.h"
#include "core/templates/local_vector.h"
#include "gaussian_gpu_layout.h"
#include "pipeline_io_contracts.h"

#include <cstdint>

// The instance-row region of an HLOD bundle, and the bundle's byte ledger, as pure device-free host code (PR1a of
// "world + movable instances in one frame"). A bundle holds N instance rows and N grading rows: row 0 is the world.
// Today exactly one row is produced (the world identity row; the route still requires an exclusive world
// submission), and for N = 1 every function here returns exactly what the single-row bundle used before.
//
// Two generations govern a prepared bundle (the director's InstanceStore counters):
// - the ASSET-SET generation (get_instance_asset_generation_for_renderer) changes what the bundle packs, so it
//   forces a rebuild;
// - the ROW generation (get_instance_generation_for_renderer) changes row content only (transform, params,
//   grading), so with an unchanged row count the rows are re-uploaded in place.
namespace gs_hlod_bundle {

// The bundle packs one asset, the world: asset id 0 at dense id 0. Rows naming any other asset are refused here; packing
// node assets (dense ids 1..M) is the next step (PR1b).
constexpr uint64_t WORLD_ASSET_ID = 0u;
constexpr uint32_t WORLD_DENSE_ID = 0u;
// The world is always row 0 (DrawnNodeGPU::instance_id of every world draw record).
constexpr uint32_t WORLD_ROW = 0u;

struct Rows {
	LocalVector<InstanceDataGPU> instances;
	LocalVector<InstanceGradingGPU> grading; // 1:1 with instances.
	LocalVector<uint64_t> asset_ids; // 1:1 with instances: the director asset id each row names.
	uint32_t count() const { return instances.size(); }
};

// Dense id of a packed asset, or UINT32_MAX when the bundle does not pack it.
inline uint32_t dense_id_for_asset(uint64_t p_asset_id) {
	return p_asset_id == WORLD_ASSET_ID ? WORLD_DENSE_ID : UINT32_MAX;
}

// GPU-culled (canonical) bundles cull every (row, chunk) pair, so their visible-chunk list holds a slot per pair. The
// relative bundle's draw list is CPU-written and node-indexed: one entry per world node.
inline uint64_t visible_chunk_capacity(uint64_t p_node_count, bool p_relative, uint32_t p_rows) {
	return p_relative ? p_node_count : p_node_count * uint64_t(p_rows);
}

// nullptr when the rows can be packed into one bundle of `p_node_count` nodes; otherwise the reason key.
inline const char *rows_refusal(const Rows &p_rows, uint64_t p_node_count) {
	const uint32_t n = p_rows.count();
	if (n == 0) { return "hlod_bundle_rows_empty"; }
	if (p_rows.grading.size() != n || p_rows.asset_ids.size() != n) { return "hlod_bundle_rows_mismatched"; }
	// Every buffer of the bundle is created with a 32-bit size (hlod_create_buffer).
	if (uint64_t(n) * sizeof(InstanceDataGPU) > UINT32_MAX ||
			visible_chunk_capacity(p_node_count, false, n) * sizeof(VisibleChunkRefGPU) > UINT32_MAX) {
		return "hlod_bundle_rows_exceed_capacity";
	}
	for (uint32_t i = 0; i < n; ++i) {
		if (dense_id_for_asset(p_rows.asset_ids[i]) == UINT32_MAX) { return "hlod_bundle_row_asset_not_packed"; }
	}
	return nullptr;
}

// The instance rows exactly as the bundle uploads them: each row names its asset's dense id (ids[0]); every other byte
// is the director's row. Only valid for rows that passed rows_refusal().
inline void pack_instance_rows(const Rows &p_rows, LocalVector<InstanceDataGPU> &r_packed) {
	r_packed = p_rows.instances;
	for (uint32_t i = 0; i < r_packed.size(); ++i) {
		r_packed[i].ids[0] = dense_id_for_asset(p_rows.asset_ids[i]);
	}
}

// Bytes of one HLOD bundle: atlas records, per-node tables, the visible/draw list, one AssetMetaGPU, N instance and N
// grading rows, the splat-ref (and relative draw-entry) capacity, counters and two indirect headers. `p_records` is the
// atlas record count (whole tree, or the streamed atlas). With p_rows == 1 this is the single-row ledger unchanged.
inline uint64_t bundle_bytes(uint64_t p_node_count, uint64_t p_records, bool p_relative, bool p_quantized, uint32_t p_capacity,
		uint32_t p_rows) {
	return p_records * (p_quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian)) +
			p_node_count * (sizeof(ChunkMetaGPU) + sizeof(AssetChunkIndexGPU) + (p_quantized ? sizeof(ChunkQuantizationGPU) : 0)) +
			visible_chunk_capacity(p_node_count, p_relative, p_rows) * (p_relative ? sizeof(DrawnNodeGPU) : sizeof(VisibleChunkRefGPU)) +
			sizeof(AssetMetaGPU) + uint64_t(p_rows) * (sizeof(InstanceDataGPU) + sizeof(InstanceGradingGPU)) +
			uint64_t(p_capacity) * (sizeof(SplatRefGPU) + (p_relative ? sizeof(uint32_t) : 0)) +
			6 * sizeof(uint32_t) + 2 * sizeof(GaussianSplatting::IndirectDispatchLayout);
}

enum class PreparedRows {
	CURRENT, // Nothing changed: publish as is.
	REUPLOAD, // Row content changed, asset set and row count did not: re-upload the rows in place.
	REBUILD, // The asset set or the row count changed: the bundle no longer describes the scene.
};

// `p_row_count` is the current row count; it is only read when the row generation moved.
inline PreparedRows prepared_rows_action(uint64_t p_prepared_asset_generation, uint64_t p_asset_generation,
		uint64_t p_prepared_row_generation, uint64_t p_row_generation, uint32_t p_prepared_rows, uint32_t p_row_count) {
	if (p_prepared_asset_generation != p_asset_generation) { return PreparedRows::REBUILD; }
	if (p_prepared_row_generation == p_row_generation) { return PreparedRows::CURRENT; }
	return p_row_count == p_prepared_rows ? PreparedRows::REUPLOAD : PreparedRows::REBUILD;
}

} // namespace gs_hlod_bundle

#endif // GS_HLOD_BUNDLE_ROWS_H
