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
	// Grouped leaves select octants of record.cell. Index-split leaves instead
	// select a sorted range in Context::selected_indices. Other nodes use 0xff.
	uint8_t octant_mask = 0xff;
	bool indexed = false;
	uint32_t selection_first = 0;
	BakeResult tile;
	SplatList merged;
};

struct Context {
	const TiledBakeSource &source;
	const TiledBakeParams &params;
	TiledBakeReport &report;
	TiledBakeSourceInfo info;
	LocalVector<Chunk> chunks; // enumeration order, immutable after descriptors
	LocalVector<uint32_t> ordered_chunks;
	LocalVector<PlanNode> plan; // fixed before workers; one writer per work node
	LocalVector<uint32_t> selected_indices;
	uint64_t node_bound = 0;
	uint64_t payload_bytes = 0;
	uint64_t decode_bytes = 0;
	uint64_t live_reserved = 0; // coordinator only; worker reservations are fixed
	std::atomic<int> failure{ OK }; // first failure, shared with every worker

	Context(const TiledBakeSource &p_source, const TiledBakeParams &p_params, TiledBakeReport &r_report) :
			source(p_source), params(p_params), report(r_report) {}
	Error fail(Error p_code);
	Error check();
	static Error check_job(void *p_context) { return static_cast<Context *>(p_context)->check(); }
	bool reserve(uint64_t p_bytes);
	void release(uint64_t p_bytes);
	Error read(uint32_t p_chunk, TiledBakeChunk &r_chunk);
	OfflineJob job(uint32_t p_count) const;
};

// Same upper bound for metadata admission and each tile's OfflineJob. Large
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
SplatSpan root_payload(const PlanNode &p_node, uint32_t p_high);
Error stitch_and_assemble(Context &p_context, BakeResult &r_result);
Error bake_tiles(Context &p_context);

} // namespace tiled_bake
} // namespace gs_hlod

#endif // GAUSSIAN_SPLAT_HLOD_TILED_INTERNAL_H
