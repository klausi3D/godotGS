#ifndef GAUSSIAN_SPLAT_HLOD_BAKE_ADMISSION_H
#define GAUSSIAN_SPLAT_HLOD_BAKE_ADMISSION_H

// D2-1 admission and failure cleanup, kept separate from the rule-1 bake math.
#include "core/error/error_list.h"

class String;
struct GaussianSplatHlodCell;

namespace gs_hlod {

struct BakeInput;
struct BakeParams;
struct BakeResult;
struct OfflineJob;

namespace bake_admission {

// Ordinary lattice cells only. Bounds are centre bounds, not Gaussian extents;
// the upper face is excluded. No automatic root choice or cell shrinking.
bool forced_root_contains(const GaussianSplatHlodCell &p_cell, const double p_origin[3],
		const double p_min[3], const double p_max[3]);

// Called only for MERGE_ONLY under an active job. Requires frozen rule-1 params
// and a successful reservation matching this input, before any bake allocation.
Error merge_only_job(const BakeInput &p_input, const BakeParams &p_params,
		const OfflineJob &p_job, String *r_error);

// New bake modes publish all or nothing, including on direct false returns from
// allocation helpers. nullptr leaves the legacy result/failure behavior alone.
class ResultRollback {
	BakeResult *result;

public:
	explicit ResultRollback(BakeResult *p_result) : result(p_result) {}
	ResultRollback(const ResultRollback &) = delete;
	ResultRollback &operator=(const ResultRollback &) = delete;
	~ResultRollback();
	void commit() { result = nullptr; }
};

} // namespace bake_admission
} // namespace gs_hlod

#endif // GAUSSIAN_SPLAT_HLOD_BAKE_ADMISSION_H
