#ifndef GS_HLOD_RUNTIME_SELECTION_H
#define GS_HLOD_RUNTIME_SELECTION_H

#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "core/math/plane.h"

namespace gs_hlod_runtime {

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
	enum Status { COMPLETE, EMPTY, INVALID, UNAVAILABLE, BOOTSTRAP_EXHAUSTED, BOOTSTRAP_ARITHMETIC,
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
	Result select(const GaussianSplatHlodRuntimeData &p_data, const CutRequest &p_request);
	uint32_t selected_node(uint32_t p_index) const { return selected[p_index]; }
	uint64_t allocated_bytes() const;

private:
	struct Candidate { uint32_t node = 0; double priority = 0.0; };
	Vector<Candidate> heap;
	Vector<uint32_t> selected;
	Vector<uint32_t> selected_position;
	Vector<uint64_t> refined;
	uint64_t epoch = 0;
	uint32_t capacity = 0;
};

}
#endif
