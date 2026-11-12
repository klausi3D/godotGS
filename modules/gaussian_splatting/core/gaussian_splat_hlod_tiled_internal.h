#ifndef GAUSSIAN_SPLAT_HLOD_TILED_INTERNAL_H
#define GAUSSIAN_SPLAT_HLOD_TILED_INTERNAL_H

#include "gaussian_splat_hlod_job.h"
#include "gaussian_splat_hlod_tiled_bake.h"

namespace gs_hlod {
namespace tiled_bake {

struct Chunk {
	TiledBakeChunkInfo info;
	uint32_t source_id = 0;
};

struct PlanNode {
	GaussianSplatHlodNode record;
	uint32_t children[kMaxChildren] = {};
	uint32_t input_count = 0;
	uint32_t depth = 0;
	bool work = false;
	// Grouped leaves select octants of record.cell. Index-split leaves select a
	// (Morton, global-index) interval, with exclusive lower/inclusive upper ends.
	uint8_t octant_mask = 0xff;
	bool indexed = false;
	uint64_t selection_low_key = 0;
	uint64_t selection_high_key = 0;
	uint32_t selection_low_global = 0;
	uint32_t selection_high_global = 0;
	bool selection_has_lower = false;
	BakeResult tile;
	uint32_t extra_id_first = 0; // non-root tile records; root uses this plan's ID
};

struct Context {
	const TiledBakeSource &source;
	const TiledBakeParams &params;
	TiledBakeReport &report;
	TiledBakeSink &sink;
	TiledBakeSourceInfo info;
	LocalVector<Chunk> chunks; // enumeration order, immutable after descriptors
	LocalVector<uint32_t> ordered_chunks;
	LocalVector<PlanNode> plan; // fixed before workers; one writer per work node
	uint64_t node_bound = 0;
	uint64_t payload_bytes = 0;
	uint64_t decode_bytes = 0;
	uint64_t live_reserved = 0; // coordinator only; worker reservations are fixed
	uint64_t metadata_reserved = 0; // coordinator only; includes sink's fixed/table owners
	uint32_t total_nodes = 0; // coordinator only; plan IDs plus emitted tile records
	std::atomic<int> failure{ OK }; // first failure, shared with every worker

	Context(const TiledBakeSource &p_source, const TiledBakeParams &p_params, TiledBakeReport &r_report, TiledBakeSink &p_sink) :
			source(p_source), params(p_params), report(r_report), sink(p_sink) {}
	Error fail(Error p_code);
	Error check();
	static Error check_job(void *p_context) { return static_cast<Context *>(p_context)->check(); }
	bool reserve(uint64_t p_bytes);
	void release(uint64_t p_bytes);
	bool reserve_metadata(uint32_t p_nodes);
	Error read(uint32_t p_chunk, TiledBakeChunk &r_chunk);
	OfflineJob job(uint32_t p_count) const;
};

// Theoretical tree-size validator and each tile's OfflineJob bound. Resident
// metadata is charged by actual node count, not this conservative bound. Large
// interiors have > leaf_max descendants, so at any depth there are at most
// floor(N/(leaf_max+1)). Index splitting can additionally emit a small unary
// interior over one chunk. There is at most one such interior per index chunk;
// disjoint index roots have at most 2*ceil(N/leaf_max) chunks in total. Every
// interior has at most eight children; height is bounded by kMaxHeight.
inline uint64_t node_bound(uint32_t p_count) {
	if (p_count <= kMaxNodeSplats) {
		return 1u;
	}
	const uint64_t large = uint64_t(p_count / (kMaxNodeSplats + 1u)) * kMaxHeight;
	const uint64_t small = 2u * ((uint64_t(p_count) + kMaxNodeSplats - 1u) / kMaxNodeSplats);
	return 1u + (large + small) * kMaxChildren;
}

// Bounds/radius/height from own payload and the children's stored records,
// using the exact shared in-core rounding primitives. Leaf payload is absolute.
void finish_metrics(GaussianSplatHlodNode &r_node, const SplatSpan &p_own,
		const GaussianSplatHlodNode *const *p_children, uint32_t p_child_count);
Error emit_tile(Context &p_context, uint32_t p_plan);
uint64_t index_key(const GaussianSplatHlodNode &p_node, const Vector3 &p_position);
Error stitch_and_assemble(Context &p_context);
Error bake_tiles(Context &p_context);

} // namespace tiled_bake
} // namespace gs_hlod

#endif // GAUSSIAN_SPLAT_HLOD_TILED_INTERNAL_H
