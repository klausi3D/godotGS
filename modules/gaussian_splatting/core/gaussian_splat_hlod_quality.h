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
// Fixed resource ceilings for the CPU metric, not total bake memory or elapsed time.
struct QualityResourceLimits {
	uint64_t image_payload_bytes = uint64_t(1) << 26u;
	uint64_t scratch_peak_bytes = uint64_t(1) << 28u;
	uint64_t node_work = uint64_t(1) << 28u;
	uint64_t bake_work = uint64_t(1) << 34u;
};
QualityResourceLimits quality_resource_limits();
// Five slots: reference/MERGED/SELECTED projections, reference/candidate images.
// Counts describe the old and replacement owners; both allocations and headers count.
bool quality_metric_memory_fits(const uint64_t p_old[5], const uint64_t p_next[5],
                                const QualityResourceLimits &p_limits);
bool quality_charge_product(uint64_t &r_remaining, uint64_t p_a, uint64_t p_b);
#ifdef TESTS_ENABLED
// Thread-local and test-only: can only lower the native size_t limit. No large allocation
// is required to exercise the real resize admission and public bake failure path.
uint64_t quality_set_test_allocation_size_max(uint64_t p_size_max);
QualityResourceLimits quality_set_test_resource_limits(const QualityResourceLimits &p_limits);
// One-shot, thread-local and matched to a static site label. Injects a null
// writable acquisition, not a process/global allocator failure.
void quality_force_writable_failure_at(const char *p_where);
bool quality_writable_failure_is_armed();
bool quality_consume_writable_failure(const char *p_where);
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
	if (!gs_resize_or_fail(p_vector, int64_t(p_count), p_where)) {
		return false;
	}
	if (p_count) {
		// Same-size resize does not fork an aliased CowData. Acquire/check the
		// writable storage here, so later ptrw()/write[] use an exclusive owner.
		T *writable = nullptr;
#ifdef TESTS_ENABLED
		if (!quality_consume_writable_failure(p_where))
#endif
		{
			writable = p_vector.ptrw();
		}
		if (writable == nullptr) {
			p_vector.clear();
			ERR_FAIL_V_MSG(false, vformat("%s: cannot acquire writable HLOD quality storage (%d entries).",
			                              String(p_where), p_count));
		}
	}
	return true;
}

// Rules 2 and 3: deterministic orthographic CPU proxy, not the perspective GPU renderer.
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
	uint64_t work_units = 0; // Declared pixel visits, excludes projection and sorting.
};
struct QualityResult {
	QualityViewResult views[kQualityViewCount];
	float merged_error = 0, selected_error = 0;
	bool use_merged = true;
};
// Rule 3: a node is drawn whenever its geometric error projects to at most `tau` pixels, and the
// runtime `tau` is a user budget, so one bake decision serves every `tau` the ADR evaluates (1, 2
// and 4 px, section 5.2). Each candidate is scored once per `tau` at pixel pitch `eps / tau`, and
// the per-`tau` errors are combined by their geometric mean: the acceptance gate reports PSNR, a
// logarithm of the error, so the decision that minimises the expected dB loss over the budgets
// compares the mean of log errors. `kQualityErrorFloor` is the MSE of 8-bit output rounding
// alone; errors below it are indistinguishable on screen and must not send a logarithm to -inf.
static constexpr uint32_t kQualityTauCount = 3u;
static constexpr double kQualityTau[kQualityTauCount] = {1.0, 2.0, 4.0};
static constexpr double kQualityErrorFloor = 1.0 / (255.0 * 255.0 * 12.0);
struct QualityBandResult {
	QualityResult taus[kQualityTauCount]; // per-`tau` 22-view results, in kQualityTau order
	float merged_error = 0, selected_error = 0; // geometric means over `tau` (floored)
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
                      uint32_t p_first_count, double p_eps, QualityScratch &r_scratch, QualityResult &r_result,
                      uint64_t *r_bake_work_remaining = nullptr);
// Rule-3 candidate comparison: runs quality_compare at pitch eps / tau for every kQualityTau
// under ONE node raster budget (and the shared bake budget), then decides on the geometric means.
Error quality_compare_band(const SplatSpan &p_reference, const SplatSpan &p_merged, const SplatSpan &p_selected,
                           uint32_t p_first_count, double p_eps, QualityScratch &r_scratch, QualityBandResult &r_result,
                           uint64_t *r_bake_work_remaining = nullptr);

} // namespace gs_hlod
#endif
