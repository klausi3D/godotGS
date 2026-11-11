#include "gaussian_splat_hlod_bake_admission.h"

#include "gaussian_splat_hlod_bake.h"
#include "gaussian_splat_hlod_job.h"

#include <cmath>

namespace gs_hlod {
namespace bake_admission {

bool forced_root_contains(const GaussianSplatHlodCell &p_cell, const double p_origin[3],
		const double p_min[3], const double p_max[3]) {
	if (p_cell.e < kMinCellExponent || p_cell.e > kMaxCellExponent) {
		return false;
	}
	const int64_t indices[3] = { p_cell.ix, p_cell.iy, p_cell.iz };
	const double edge = std::ldexp(1.0, p_cell.e);
	for (int a = 0; a < 3; ++a) {
		// Compare signed bounds directly: abs(INT64_MIN) would overflow.
		if (indices[a] <= -kMaxCellIndexMagnitude || indices[a] >= kMaxCellIndexMagnitude ||
				!std::isfinite(p_origin[a]) || !std::isfinite(p_min[a]) || !std::isfinite(p_max[a])) {
			return false;
		}
		const double lower = p_origin[a] + double(indices[a]) * edge;
		const double upper = lower + edge;
		if (!std::isfinite(lower) || !std::isfinite(upper) || !(lower < upper) ||
				p_min[a] > p_max[a] || p_min[a] < lower || p_max[a] >= upper) {
			return false;
		}
	}
	return true;
}

Error merge_only_job(const BakeInput &p_input, const BakeParams &p_params,
		const OfflineJob &p_job, String *r_error) {
	auto fail = [&](const char *p_message, Error p_code) {
		if (r_error) {
			*r_error = p_message;
		}
		return p_code;
	};
	// Freeze the complete rule-1 parameter set, not just the reduction used in
	// the reservation bound. QUALITY_SELECTION keeps its existing admission.
	const InteriorBakeParams &interior = p_params.interior;
	if (p_params.leaf_max_splats != kMaxNodeSplats || interior.node_max_splats != kMaxNodeSplats ||
			interior.reduction != 4.0 || interior.merge_grid != 64u ||
			interior.eps_growth != 1.25 || interior.max_eps_steps != 1024u) {
		return fail("Offline MERGE_ONLY job requires the frozen rule-1 bake parameters.", ERR_INVALID_DATA);
	}
	if (p_input.sh_first_order_count > 3u || p_input.sh_high_order_count > 21u) {
		return fail("Offline MERGE_ONLY job supports compact SH0 through SH4 only.", ERR_INVALID_DATA);
	}
	if (!p_job.reservation_valid || p_job.admitted_count != p_input.count ||
			p_job.admitted_high != p_input.sh_high_order_count || p_job.reserved == 0u ||
			p_job.reserved > p_job.scratch || p_input.count > p_job.input || p_input.count > p_job.prepared ||
			p_job.input > UINT32_MAX || p_job.prepared > UINT32_MAX || p_job.nodes == 0u || p_job.nodes > UINT32_MAX) {
		return fail("Offline MERGE_ONLY job needs a matching successful RAM reservation and bounded counts.", ERR_OUT_OF_MEMORY);
	}
	// Recheck the current caps without changing the caller's reservation. The
	// public job fields may have changed since reserve(); zero old-interior bytes
	// gives the minimum core-bake charge (the caller may have reserved more).
	OfflineJob minimum = p_job;
	if (!minimum.reserve(p_input.count, p_input.sh_high_order_count, 0u) || p_job.reserved < minimum.reserved) {
		return fail("Offline MERGE_ONLY job reservation does not cover the current bake limits.", ERR_OUT_OF_MEMORY);
	}
	const Error stopped = p_job.check();
	if (stopped != OK) {
		return fail("Offline MERGE_ONLY job stopped before baking.", stopped);
	}
	return OK;
}

ResultRollback::~ResultRollback() {
	if (result) {
		*result = BakeResult();
	}
}

} // namespace bake_admission
} // namespace gs_hlod
