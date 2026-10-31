#ifndef GAUSSIAN_SPLAT_HLOD_BAKE_H
#define GAUSSIAN_SPLAT_HLOD_BAKE_H

// HLOD bake (ADR docs/architecture/adr-hlod-streaming.md §6.1-§6.3, slices S1a/S1b): builds the
// world-aligned octree over a world's splats, chooses merged/selected interior nodes bottom-up and lays the payload
// out node-contiguous and importance-ordered. Pure CPU, host-testable; no Resource or GPU state.
//
// The lattice is the world frame's (origin O = (0, 0, 0) for a world without one, see
// gaussian_splat_hlod_tree.h); the root is the smallest lattice cell holding every centre, or the
// origin-centred cube when the content straddles a plane through O.

#include "gaussian_splat_hlod_merge.h"
#include "gaussian_splat_hlod_tree.h"
#ifdef TESTS_ENABLED
#include "gaussian_splat_hlod_quality.h"
#endif

namespace gs_hlod {

#ifdef TESTS_ENABLED
// Test-only, explicit opt-in candidate capture (S1b matched-arm quality exporter, ADR §9 S1b).
// At every interior node of a QUALITY_SELECTION bake the baker builds a MERGED (SAT) candidate and
// a SELECTED (A-area) candidate of the same count and keeps one. With a capture attached, BOTH are
// kept here, together with the winner flag, before either is discarded. Positions are node-relative
// (the node's cell centre), exactly as the baker holds them. The existing payload types are reused;
// no file format is added. The capture never changes the baked tree: it only copies/shares.
struct CandidateCapture {
	struct Node {
		SplatList sat;         // MERGED candidate (rule 1 `sat`), children = the baker's chosen child payloads
		QualityPayload area;   // SELECTED candidate (A-area leaf-prefix selection), same count as `sat`
		bool use_merged = true; // true: the SAT candidate won (S1b choice)
		bool captured = false;
	};
	// REQUIRED, nonzero: byte ceiling for everything the capture retains (both candidates of all
	// interior nodes). Exceeding it fails the bake closed; nothing is truncated or dropped.
	uint64_t max_bytes = 0;
	LocalVector<Node> nodes; // indexed by tree node index; filled for interior nodes only
	uint64_t captured_bytes = 0;
	uint32_t captured_nodes = 0;
};
#endif

struct BakeParams {
	enum Mode { MERGE_ONLY, QUALITY_SELECTION };
	Mode mode = MERGE_ONLY; // Canonical imports and the public bake retain rule 1.
	uint32_t leaf_max_splats = kMaxNodeSplats; // <= kMaxNodeSplats; tests use smaller leaves
	InteriorBakeParams interior;
#ifdef TESTS_ENABLED
	// Test-only. nullptr (the default) is the production path: the capture branch is a single
	// pointer test per interior node and the baked tree is byte-identical either way.
	CandidateCapture *capture = nullptr;
#endif
};

struct BakeInput {
	const Gaussian *gaussians = nullptr; // absolute positions, as the loaders produce them
	const Vector3 *sh_high_order = nullptr; // count * sh_high_order_count, may be null if 0
	uint32_t count = 0u;
	uint32_t sh_high_order_count = 0u;
	uint32_t sh_first_order_count = 0u; // explicit compact prefix; high_count alone cannot distinguish SH0/SH1
};

struct BakeResult {
	// Nodes, grid, counts and the resident interior payload (node-relative positions).
	GaussianSplatHlodTree tree;
	// The leaf section in payload order, ABSOLUTE positions (bit-identical to the input splats),
	// for today's runtime; the saver makes them node-relative on disk.
	LocalVector<Gaussian> leaf_gaussians;
	LocalVector<Vector3> leaf_sh_high_order;
	// leaf_source_index[i] = index in the input of leaf payload splat i (a permutation).
	LocalVector<uint32_t> leaf_source_index;
};

// Chooses the root for centres within [p_min, p_max] on the lattice with origin p_origin (§6.1):
// the smallest cell (e >= -16) holding them all, or, if they straddle a plane through the origin,
// the origin-centred cube [O - 2^e, O + 2^e) (r_origin_centred = true, r_cell = (e, 0, 0, 0)).
// Returns false when nothing up to 2^62 m covers them (non-finite or absurd input).
bool choose_root_cell(const double p_origin[3], const double p_min[3], const double p_max[3],
		GaussianSplatHlodCell &r_cell, bool &r_origin_centred);

// Bakes p_input. Fails closed (returns false, r_error set) on empty input, non-finite splat
// fields (position, scale, rotation, opacity, SH), a leaf size outside [1, 16384], a payload
// that would not fit the 32-bit index space, or a splat-count-sized allocation that cannot be
// served. r_code, when given, receives ERR_OUT_OF_MEMORY for the last and ERR_INVALID_DATA
// otherwise (numeric image-domain range failures retain their typed error). The interior merge's per-node allocations
// are probed too (the bound comment in gaussian_splat_hlod_merge.h); the node-count-sized build stacks and the <=
// kMaxChildren child spans are not.
bool bake_world(const BakeInput &p_input, const BakeParams &p_params, BakeResult &r_result, String *r_error,
		Error *r_code = nullptr);

} // namespace gs_hlod

#endif // GAUSSIAN_SPLAT_HLOD_BAKE_H
