#ifndef GAUSSIAN_SPLAT_HLOD_QUALITY_H
#define GAUSSIAN_SPLAT_HLOD_QUALITY_H

#include "core/templates/vector.h"
#include "gaussian_splat_hlod_merge.h"
#include "gaussian_splat_hlod_tree.h"
#include "gs_vector_alloc.h"

namespace gs_hlod {

// Local admission for the actual CowData allocation, not just count * sizeof(T).
// Keep this bounded to new quality scratch; it does not change the common allocator.
bool quality_allocation_fits(uint64_t p_count, uint64_t p_element_bytes, uint64_t p_size_max, uint64_t p_header_bytes);
uint64_t quality_allocation_header_bytes();
uint64_t quality_allocation_size_max();
#ifdef TESTS_ENABLED
// Thread-local and test-only: can only lower the native size_t limit. No large allocation
// is required to exercise the real resize admission and public bake failure path.
uint64_t quality_set_test_allocation_size_max(uint64_t p_size_max);
#endif

template <typename T>
[[nodiscard]] bool quality_resize_or_fail(Vector<T> &p_vector, uint64_t p_count, const char *p_where)
{
	if (!quality_allocation_fits(p_count, sizeof(T), quality_allocation_size_max(),
	                             quality_allocation_header_bytes())) {
		p_vector.clear();
		ERR_FAIL_V_MSG(
		        false,
		        vformat("%s: rounded HLOD quality allocation (%d elements of %d bytes plus header) exceeds size_t.",
		                String(p_where), p_count, uint64_t(sizeof(T))));
	}
	return gs_resize_or_fail(p_vector, int64_t(p_count), p_where);
}

// Rule 2: deterministic orthographic CPU proxy, not the perspective GPU renderer.
static constexpr uint32_t kQualityViewCount = 22u;
struct QualityView {
	double eye[3] = {};
	double right[3] = {};
	double up[3] = {};
	uint32_t pitch_multiplier = 1u;
};
struct QualityPixel {
	double rgba[4] = {};
};
struct QualityProjected {
	double x = 0, y = 0, rx = 0, ry = 0, depth = 0;
	double conditional_inverse = 0, xy_over_yy = 0, yy_inverse = 0;
	double alpha = 0, rgb[3] = {};
	uint32_t ordinal = 0;
};
struct QualityLeafShare {
	uint32_t node = 0, count = 0, share = 0;
	uint64_t first = 0, remainder = 0;
};
struct QualityOrder {
	float importance = 0;
	uint32_t ordinal = 0;
};
struct QualityPayload {
	Vector<Gaussian> gaussians;
	Vector<Vector3> sh;
	uint32_t high_count = 0;
	uint32_t size() const { return uint32_t(gaussians.size()); }
	SplatSpan span() const { return {gaussians.ptr(), sh.ptr(), size(), high_count}; }
};
struct QualityScratch {
	Vector<QualityProjected> projected[3];
	Vector<QualityPixel> reference_image, candidate_image;
	Vector<QualityLeafShare> shares;
	Vector<QualityOrder> order;
};
struct QualityViewResult {
	int64_t x0 = 0, y0 = 0;
	uint64_t width = 0, height = 0;
	double merged_mse = 0, selected_mse = 0;
};
struct QualityResult {
	QualityViewResult views[kQualityViewCount];
	float merged_error = 0, selected_error = 0;
	bool use_merged = true;
};

QualityView quality_view(uint32_t p_index);
Error quality_color(const Gaussian &p_gaussian, const Vector3 *p_high, uint32_t p_first_count, uint32_t p_high_count,
                    const double p_ray[3], double r_rgb[3]);
bool quality_prefer_merged(double p_merged, double p_selected);
// Allocates exact largest-remainder shares, ties by full leaf node id, then copies
// original leaf importance prefixes and orders the resulting candidate by importance.
Error quality_select(const GaussianSplatHlodTree &p_tree, const Vector<uint32_t> &p_dfs_leaves, uint32_t p_first_leaf,
                     uint32_t p_leaf_count, const SplatSpan &p_leaves, uint32_t p_count, const double p_center[3],
                     QualityScratch &r_scratch, QualityPayload &r_payload);
// Retains reference and SELECTED images in scratch for analytic native tests.
Error quality_compare_view(const SplatSpan &p_reference, const SplatSpan &p_merged, const SplatSpan &p_selected,
                           uint32_t p_first_count, double p_eps, uint32_t p_view, QualityScratch &r_scratch,
                           QualityViewResult &r_result);
Error quality_compare(const SplatSpan &p_reference, const SplatSpan &p_merged, const SplatSpan &p_selected,
                      uint32_t p_first_count, double p_eps, QualityScratch &r_scratch, QualityResult &r_result);

} // namespace gs_hlod
#endif
