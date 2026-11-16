#ifndef GS_HLOD_RUNTIME_SELECTION_H
#define GS_HLOD_RUNTIME_SELECTION_H

#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "core/math/plane.h"

namespace gs_hlod_runtime {

// Overlap-record estimate (ADR 6.5): the distance used for a node's footprint term is never smaller than this fraction
// of the node's bounding radius, so a camera inside a node's sphere (or in the shell up to radius x 1.25 outside it) gets
// a finite estimate. A heuristic, not a bound (see CutHistory::select()).
constexpr double kCostInsideDistanceFraction = 0.25;
// The admission factor of the overlap-record cap (ADR 6.5): a cut is admitted while its record estimate stays within this
// fraction of the record budget (the allocated overlap storage). The estimate is a heuristic, so the rest is its margin.
constexpr double kRecordAdmissionFactor = 0.8;

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
	// Page admission for the cut and residency demand (ADR 6.6): when page_splats != 0, a node occupies
	// ceil(payload / page) atlas pages. Every transition must fit page_budget together with the cut, every
	// ancestor of it, and already demanded transitions, leaving the largest non-stale node's run as reserve.
	// Unprepared snapshots retain option-1 admission without reserve. Parents stay charged during overlap.
	// The mandatory root uses physical page_budget.
	uint32_t page_splats = 0;
	uint64_t page_budget = 0;
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
	enum PlacementReason { PLACEMENT_NONE, RESERVE_ROOT_ONLY };
	// Stable report-only trace codes: absent -> null/-1; unprepared -> option-1 physical-page admission.
	enum PlacementReserveState { RESERVE_ABSENT = -1, RESERVE_UNPREPARED = 0, RESERVE_MEASURED = 1 };
	struct Result {
		SelectStatus status = INVALID;
		PlacementReason placement_reason = PLACEMENT_NONE; // COMPLETE root retained when reserve prevents expansion.
		uint32_t selected_count = 0;
		uint32_t visited_nodes = 0;
		uint64_t selected_splats = 0;
		double estimated_records = 0.0;
		bool budget_bound = false; // Existing splat/node/record selection caps only; pages use page_refused_groups.
		// #1472: the record estimate of the largest step the RECORD cap alone refused (0: it refused none): a refinement
		// or a residency demand whose splats, nodes and pages fit, or the cheapest bootstrap frontier (the root included)
		// that fits every other cap. The overlap storage is the record budget, so this is what a storage sized below the view's demand
		// costs the cut; the route grows the storage from it instead of leaving the cut coarse. A refusal by another
		// cap reports nothing: more records would not admit it.
		double record_demand = 0.0;
		bool bootstrapped = false; // Root failed admission and a child frontier was selected instead.
		bool edit_forced = false; // Package F1: stale ancestors of edited leaves were replaced by their children (ADR 14.5).
		// Report-only diagnostics of this invocation. A refusal includes a demand-plan/page cap or
		// bootstrap node cap; budget_bound above retains its existing selection semantics.
		int32_t first_cap_refused_node = -1;
		uint32_t hysteresis_held_count = 0;
		uint32_t residency_missed_count = 0;
		// Prepared snapshots measure headroom; unprepared snapshots use zero reserve without claiming a measurement.
		PlacementReserveState placement_reserve_state = RESERVE_ABSENT;
		bool placement_reserve_measured = false;
		uint64_t placement_reserve_pages = 0;
		// Report-only union of the current cut, its ancestors, and every visible child of an
		// issued transition (including resident siblings). Parents remain charged during overlap.
		// These fields never participate in selection or admission. page_splats == 0 is unmeasured.
		bool closure_measured = false;
		uint64_t demanded_closure_pages = 0;
		uint64_t page_budget = 0;
		uint64_t bootstrap_bypass_pages = 0;
		uint64_t resident_refinement_bypass_pages = 0;
		bool bootstrap_bypass_over_budget = false;
		bool resident_refinement_bypass_over_budget = false;
		uint32_t page_refused_groups = 0; // Ready or missing child groups whose page-cap predicate failed (root included).
	};
	enum DiagnosticFlag : uint8_t { HYSTERESIS_HELD = 1, RESIDENCY_MISSED = 2, MISS_DEMANDED = 4 };
	// Per-node flags, valid until the next select/invalidate/reserve. HYSTERESIS_HELD means an
	// ordinary transition actually taken solely in the history band, never a bootstrap expansion.
	uint8_t diagnostic_flags(uint32_t p_node) const { return diagnostics[p_node] & (HYSTERESIS_HELD | RESIDENCY_MISSED | MISS_DEMANDED); }
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
	// Selection workspace charge (CowData-rounded); 0 if the count overflows. The existing diagnostic
	// byte (also holding the page-plan bit) stays excluded to preserve the charge; allocated_bytes() includes it.
	static uint64_t workspace_bytes(uint32_t p_nodes);

private:
	// Spare bits in already admitted diagnostic scratch; no additional allocation or admission charge.
	enum PageDiagnosticFlag : uint8_t { PAGE_ACCOUNTED = 8, BOOTSTRAP_PAGE_ACCOUNTED = 16, RESIDENT_PAGE_ACCOUNTED = 32 };
	// Separate policy bit in the same preallocated byte: an emitted group's pages are reserved exactly once.
	enum PagePlanFlag : uint8_t { TRANSITION_PLANNED = 64 };
	struct Candidate { uint32_t node = 0; uint32_t tie = 0; double priority = 0.0; };
	Vector<Candidate> heap;
	Vector<uint32_t> selected;
	Vector<uint32_t> selected_position;
	Vector<uint64_t> refined;
	Vector<uint32_t> demand;
	Vector<uint8_t> diagnostics; // Preallocated; cleared per selection. Only TRANSITION_PLANNED participates in admission.
	uint32_t demand_size = 0;
	uint32_t partial_size = 0;
	uint64_t epoch = 0;
	uint32_t capacity = 0;
};

}
#endif
