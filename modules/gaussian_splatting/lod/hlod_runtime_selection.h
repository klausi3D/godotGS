#ifndef GS_HLOD_RUNTIME_SELECTION_H
#define GS_HLOD_RUNTIME_SELECTION_H

#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "core/math/plane.h"

namespace gs_hlod_runtime {

// Overlap-record estimate (ADR 6.5): the distance used for a node's footprint term is never smaller than this fraction
// of the node's bounding radius, so a camera inside a node's sphere (or in the shell up to radius x 1.25 outside it) gets
// a finite estimate. A heuristic, not a bound (see CutHistory::select()).
constexpr double kCostInsideDistanceFraction = 0.25;

struct CutRequest {
	Vector3 camera;
	Plane frustum[6];
	bool frustum_enabled = true;
	double focal_pixels = 0.0;
	double tau_pixels = 1.0;
	double record_budget = 0.0;
	double tile_area = 256.0;
	uint64_t splat_budget = 0;
	uint32_t node_budget = 0;
	// Tests exercise unavailable children through the same production selection path.
	const uint8_t *resident = nullptr;
	uint32_t resident_count = 0;
	// Optional per-node tie rank (ADR 6.9, 14.1): equal-priority nodes are ordered by rank, then by index.
	// A caller that wants array-order independence passes the rank of each node's stable key. Null keeps
	// the node index as the tie-break (the pre-existing behaviour).
	const uint32_t *tie_rank = nullptr;
	uint32_t tie_rank_count = 0;
};

// One complete view history. All storage is admitted before rendering; select() never
// allocates, grows, reads GPU data or scans the complete payload/node table.
class CutHistory {
public:
	// Every non-COMPLETE/EMPTY status means "no complete cut": selected_count is 0 and callers must
	// not publish an active (empty) cut. When the root alone fails admission, a bounded bootstrap
	// (ADR section 14.2) looks for a complete visible child frontier that fits every cap; its failure
	// is reported as exactly one of the BOOTSTRAP_* statuses:
	//   BOOTSTRAP_EXHAUSTED          - the configured search-work bound ran out; says nothing about feasibility.
	//   BOOTSTRAP_ARITHMETIC         - a non-finite or overflowing cap computation was met.
	//   BOOTSTRAP_MISSING_RESIDENCY  - expansion needed a visible child that is not resident.
	//   BOOTSTRAP_INFEASIBLE         - the deterministic search expanded every admissible node and no
	//                                  frontier fits (not a proof over all frontiers; see select()).
	// Not named `Status`: X11/Xlib.h #defines Status, and this header reaches the X11 display server TU.
	enum SelectStatus { COMPLETE, EMPTY, INVALID, UNAVAILABLE, BOOTSTRAP_EXHAUSTED, BOOTSTRAP_ARITHMETIC,
		BOOTSTRAP_MISSING_RESIDENCY, BOOTSTRAP_INFEASIBLE };
	struct Result {
		SelectStatus status = INVALID;
		uint32_t selected_count = 0;
		uint32_t visited_nodes = 0;
		uint64_t selected_splats = 0;
		double estimated_records = 0.0;
		bool budget_bound = false;
		bool bootstrapped = false; // Root failed admission and a child frontier was selected instead.
	};
	bool reserve(uint32_t p_nodes);
	void invalidate();
	// Desired(view) (ADR 14.1), ordered by the selector's own refinement priority (highest first): the
	// non-resident visible children of every parent whose replacement the quality rule asked for and whose
	// whole transition fits every cap, plus the root when it is not resident. Children of one parent are
	// consecutive. This is demand only: a listed node is never drawable until the residency layer reports it
	// resident, and the selector itself refines a parent only when every visible child is resident.
	// Valid until the next select(); preallocated, so select() still never allocates.
	uint32_t demand_count() const { return demand_size; }
	uint32_t demand_node(uint32_t p_index) const { return demand[p_index]; }
	// BOOTSTRAP_MISSING_RESIDENCY only: the resident part of the frontier the bootstrap had reached when it ran
	// out of residency (nodes the quality rule wants kept, already resident). It is not a cut and is never
	// drawn, but the residency layer must not evict it while the rest of the frontier loads: a complete group
	// is not in demand (nothing is missing), yet discarding it would undo the work every frame.
	// Valid until the next select(); 0 for every other status.
	uint32_t partial_frontier_count() const { return partial_size; }
	uint32_t partial_frontier_node(uint32_t p_index) const { return selected[p_index]; }
	Result select(const GaussianSplatHlodRuntimeData &p_data, const CutRequest &p_request);
	uint32_t selected_node(uint32_t p_index) const { return selected[p_index]; }
	uint64_t allocated_bytes() const;
	// Bytes `reserve(p_nodes)` allocates (CowData-rounded); 0 if the count overflows.
	static uint64_t workspace_bytes(uint32_t p_nodes);

private:
	struct Candidate { uint32_t node = 0; uint32_t tie = 0; double priority = 0.0; };
	Vector<Candidate> heap;
	Vector<uint32_t> selected;
	Vector<uint32_t> selected_position;
	Vector<uint64_t> refined;
	Vector<uint32_t> demand;
	uint32_t demand_size = 0;
	uint32_t partial_size = 0;
	uint64_t epoch = 0;
	uint32_t capacity = 0;
};

}
#endif
