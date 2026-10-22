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
	enum Status { COMPLETE, EMPTY, ROOT_DROPPED, INVALID, UNAVAILABLE };
	struct Result {
		Status status = INVALID;
		uint32_t selected_count = 0;
		uint32_t visited_nodes = 0;
		uint64_t selected_splats = 0;
		double estimated_records = 0.0;
		bool budget_bound = false;
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
