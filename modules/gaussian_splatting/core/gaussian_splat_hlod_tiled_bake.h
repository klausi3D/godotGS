#ifndef GAUSSIAN_SPLAT_HLOD_TILED_BAKE_H
#define GAUSSIAN_SPLAT_HLOD_TILED_BAKE_H

#include "gaussian_splat_hlod_bake.h"

#include <atomic>

namespace gs_hlod {

// D2-2, host-only: a re-iterable decoded source, independent of GSQ and writers.
// Chunk enumeration order need not be global splat order. Descriptors partition
// [0, splat_count) without gaps/overlap; first_splat + local index is the stable
// global index used for every bake tie-break. Descriptors and data are immutable
// throughout build(). All source methods must support concurrent calls.
struct TiledBakeSourceInfo {
	uint32_t splat_count = 0;
	uint32_t chunk_count = 0;
	uint32_t sh_high_order_count = 0;
	uint32_t sh_first_order_count = 0;
};

struct TiledBakeChunkInfo {
	uint32_t first_splat = 0;
	uint32_t splat_count = 0;
	AABB centre_bounds; // absolute closed bounds; upper face uses AABB::get_end()
};

struct TiledBakeChunk {
	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh_high_order;
};

class TiledBakeSource {
public:
	virtual ~TiledBakeSource() = default;
	virtual TiledBakeSourceInfo get_info() const = 0;
	virtual Error get_chunk_info(uint32_t p_chunk, TiledBakeChunkInfo &r_info, String *r_error) const = 0;
	// Returns owned storage: count Gaussians and count * high_count SH entries in
	// ascending global order within this chunk. No pointers into a shared decoder.
	virtual Error read_chunk(uint32_t p_chunk, TiledBakeChunk &r_chunk, String *r_error) const = 0;
};

struct TiledBakeLimits {
	// Aggregate simultaneous-live reservation, including decoded chunks, tile
	// bakes, histogram, stitch and resident output; excludes caller-owned source.
	// Reserve before allocation using D2-1 OfflineJob's formula for each tile.
	uint64_t max_ram_bytes = 0;
	uint64_t deadline_usec = 0; // absolute OS ticks; zero is already expired
	const std::atomic<bool> *cancel = nullptr; // borrowed through synchronous build
};

#ifdef TESTS_ENABLED
struct TiledBakeTestHooks {
	// Lower-only cap to exercise histogram coarsening with small fixtures.
	uint32_t histogram_max_bins = 8192;
	// R9 mutation: retain source enumeration order when gathering tile input,
	// bypassing ascending-global-index assembly. Never change payload values or
	// global source IDs, and never apply this switch to canonical BFS assembly.
	bool bypass_input_ordering = false;
	// Called after each completed tile, before further work/publication. May run
	// on workers; callbacks and userdata must be thread safe and remain alive.
	void (*tile_completed)(void *) = nullptr;
	// Deterministic deadline forcing; nullptr uses OS::get_ticks_usec().
	uint64_t (*clock_usec)(void *) = nullptr;
	void *userdata = nullptr;
};
#endif

struct TiledBakeParams {
	uint32_t max_tile_splats = 1000000; // must be > kMaxNodeSplats
	uint32_t worker_count = 1; // concurrency ceiling; admission can run smaller batches
	uint64_t source_decoder_bytes = 0; // private source decoder scratch per concurrent read
	TiledBakeLimits limits;
#ifdef TESTS_ENABLED
	const TiledBakeTestHooks *test_hooks = nullptr;
#endif
};

// All callbacks run on the driving thread, after the entire worker batch joins.
// Payload pointers are borrowed for append_node only. Interior coordinates are
// cell-relative and leaves are absolute, exactly as in BakeResult. A frontier
// must be reloadable losslessly until finish; compressed writer bodies are not
// a substitute for this scratch copy. Provisional IDs are dense but not BFS.
class TiledBakeSink {
public:
	virtual ~TiledBakeSink() = default;
	virtual uint64_t reservation_bytes(uint32_t p_total_nodes) const = 0;
	virtual Error append_node(uint32_t p_id, const GaussianSplatHlodNode &p_node,
			uint32_t p_depth, const SplatSpan &p_payload, const uint32_t *p_leaf_source_indices, bool p_frontier) = 0;
	virtual Error load_frontier(uint32_t p_id, TiledBakeChunk &r_payload) = 0;
	virtual Error finish(GaussianSplatHlodTree &p_tree, const LocalVector<uint32_t> &p_bfs_to_provisional) = 0;
};

struct TiledBakeReport {
	uint32_t tiles_baked = 0;
	uint32_t largest_tile_splats = 0;
	uint32_t stitch_levels = 0; // levels above the deepest frontier cell
	uint32_t histogram_passes = 0; // includes capacity-triggered coarser retries
	uint64_t peak_reserved_bytes = 0; // aggregate, not a per-worker maximum
};

class TiledAutoTree {
public:
	// Bounded production path. Completed tile payloads leave the coordinator
	// immediately; only topology and sink-owned frontier scratch survive a batch.
	static Error build_streamed(const TiledBakeSource &p_source, const TiledBakeParams &p_params,
			TiledBakeSink &p_sink, TiledBakeReport &r_report, String *r_error = nullptr);
	// Pass 1 counts centres and chooses an adaptive frontier; pass 2 admits and
	// bakes each tile with its forced lattice root and frozen MERGE_ONLY defaults;
	// pass 3 stitches tile-root payloads and assembles canonical BFS records.
	// This compatibility API is an in-memory sink over build_streamed and therefore
	// explicitly reserves its retained output (leaves remain DFS/importance ordered).
	// Recheck the assembled tree before publishing. File reopen/decode/W13 belongs
	// to the D2-3 writer integration, not this in-memory API.
	// Output must equal in-core bake_world byte for byte, including global leaf
	// source indices, independent of worker count and chunk enumeration order.
	// No bound script API, GSQ I/O, Tier A job or cache integration here.
	// The initial histogram is serial; overfull fine cells are refined by bounded
	// rereads. Coincident/finest-cell overflow uses the in-core index-split shape
	// with reserved Morton/global-index scratch, never an oversized spatial tile.
	//
	// Synchronous, all-or-nothing: on ANY error reset r_result, drain workers and
	// release reservations. r_report may retain diagnostics, never payloads.
	// RAM -> ERR_OUT_OF_MEMORY; deadline -> ERR_TIMEOUT; cancel -> ERR_SKIP;
	// malformed source/params -> ERR_INVALID_DATA; another offline job -> ERR_BUSY;
	// success -> OK. Check limits
	// throughout all three passes and immediately before publication.
	static Error build(const TiledBakeSource &p_source, const TiledBakeParams &p_params,
			BakeResult &r_result, TiledBakeReport &r_report, String *r_error = nullptr);
};

} // namespace gs_hlod

#endif // GAUSSIAN_SPLAT_HLOD_TILED_BAKE_H
