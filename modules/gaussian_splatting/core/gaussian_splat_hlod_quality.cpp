#include "gaussian_splat_hlod_quality.h"
#include "gs_vector_alloc.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {
namespace QualitySH {
using uint = uint32_t;
using std::min;
#define vec3 Vector3
#define GS_SH_OUT
#include "../shaders/includes/gs_sh_basis.glsl"
#undef GS_SH_OUT
#undef vec3
} // namespace QualitySH

double dot3(const double *a, const double *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
void normalize3(double *v)
{
	const double length = std::sqrt(dot3(v, v));
	for (int i = 0; i < 3; ++i) {
		v[i] /= length;
	}
}
void cross3(const double *a, const double *b, double *v)
{
	v[0] = a[1] * b[2] - a[2] * b[1];
	v[1] = a[2] * b[0] - a[0] * b[2];
	v[2] = a[0] * b[1] - a[1] * b[0];
}
double covariance_product(const double c[6], const double a[3], const double b[3])
{
	return a[0] * (c[0] * b[0] + c[1] * b[1] + c[2] * b[2]) + a[1] * (c[1] * b[0] + c[3] * b[1] + c[4] * b[2]) +
	       a[2] * (c[2] * b[0] + c[4] * b[1] + c[5] * b[2]);
}
Error project(const gs_hlod::SplatSpan &span, uint32_t first, const gs_hlod::QualityView &view, double pitch,
              Vector<gs_hlod::QualityProjected> &out, const char *label)
{
	if ((span.count && !span.gaussians) || span.sh_high_order_count > 21u || first > 3u ||
	    (span.count && span.sh_high_order_count && !span.sh_high_order)) {
		return ERR_INVALID_DATA;
	}
	if (!gs_hlod::quality_resize_or_fail(out, span.count, label)) {
		return ERR_OUT_OF_MEMORY;
	}
	gs_hlod::QualityProjected *dst = out.ptrw();
	const double ray[3] = {-view.eye[0], -view.eye[1], -view.eye[2]};
	for (uint32_t i = 0; i < span.count; ++i) {
		const Gaussian &g = span.gaussians[i];
		gs_hlod::QualityProjected p;
		p.ordinal = i;
		if (!std::isfinite(g.opacity)) {
			return ERR_INVALID_DATA;
		}
		p.alpha = std::clamp(double(g.opacity), 0.0, 0.99);
		if (gs_hlod::quality_color(
		            g, span.sh_high_order_count ? span.sh_high_order + uint64_t(i) * span.sh_high_order_count : nullptr,
		            first, span.sh_high_order_count, ray, p.rgb) != OK) {
			return ERR_INVALID_DATA;
		}
		const double position[3] = {g.position.x, g.position.y, g.position.z};
		p.x = dot3(position, view.right) / pitch;
		p.y = dot3(position, view.up) / pitch;
		p.depth = -dot3(position, view.eye); // front to back: nearest the eye first
		double cov[6];
		gs_hlod::splat_covariance(g, cov);
		const double xx = covariance_product(cov, view.right, view.right) / (pitch * pitch) + 0.3;
		const double xy = covariance_product(cov, view.right, view.up) / (pitch * pitch);
		const double yy = covariance_product(cov, view.up, view.up) / (pitch * pitch) + 0.3;
		const double det = xx * yy - xy * xy;
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.depth) || !std::isfinite(det) || !(xx > 0) ||
		    !(yy > 0) || !(det > 0)) {
			return ERR_INVALID_DATA;
		}
		p.conditional_inverse = yy / det;
		p.xy_over_yy = xy / yy;
		p.yy_inverse = 1.0 / yy;
		const double support = p.alpha >= 1.0 / 255.0 ? 2.0 * std::log(p.alpha * 255.0) : 0.0;
		p.rx = std::sqrt(support * xx);
		p.ry = std::sqrt(support * yy);
		if (!std::isfinite(p.rx) || !std::isfinite(p.ry) || !std::isfinite(p.conditional_inverse) ||
		    !std::isfinite(p.xy_over_yy) || !std::isfinite(p.yy_inverse)) {
			return ERR_INVALID_DATA;
		}
		dst[i] = p;
	}
	if (span.count > 1u) {
		std::sort(dst, dst + span.count, [](const auto &a, const auto &b) {
			return a.depth < b.depth || (a.depth == b.depth && a.ordinal < b.ordinal);
		});
	}
	return OK;
}
Error raster(const Vector<gs_hlod::QualityProjected> &projected, const gs_hlod::QualityViewResult &domain,
             Vector<gs_hlod::QualityPixel> &image, const char *label)
{
	const uint64_t pixels = domain.width * domain.height; // checked by domain admission
	if (!gs_hlod::quality_resize_or_fail(image, int64_t(pixels), label)) {
		return ERR_OUT_OF_MEMORY;
	}
	gs_hlod::QualityPixel *dst = image.ptrw();
	for (uint64_t i = 0; i < pixels; ++i) {
		dst[i] = gs_hlod::QualityPixel();
	}
	for (const gs_hlod::QualityProjected &p : projected) {
		if (p.alpha < 1.0 / 255.0) {
			continue;
		}
		const int64_t x0 = MAX(domain.x0, int64_t(std::floor(p.x - p.rx)));
		const int64_t y0 = MAX(domain.y0, int64_t(std::floor(p.y - p.ry)));
		const int64_t x1 = MIN(domain.x0 + int64_t(domain.width), int64_t(std::ceil(p.x + p.rx)));
		const int64_t y1 = MIN(domain.y0 + int64_t(domain.height), int64_t(std::ceil(p.y + p.ry)));
		for (int64_t y = y0; y < y1; ++y) {
			const double dy = double(y) + 0.5 - p.y;
			for (int64_t x = x0; x < x1; ++x) {
				const double dx = double(x) + 0.5 - p.x - p.xy_over_yy * dy;
				const double q = dx * dx * p.conditional_inverse + dy * dy * p.yy_inverse;
				const double alpha = p.alpha * std::exp(-0.5 * q);
				if (alpha < 1.0 / 255.0) {
					continue;
				}
				gs_hlod::QualityPixel &pixel = dst[uint64_t(y - domain.y0) * domain.width + uint64_t(x - domain.x0)];
				const double weight = (1.0 - pixel.rgba[3]) * alpha;
				for (int channel = 0; channel < 3; ++channel) {
					pixel.rgba[channel] += weight * p.rgb[channel];
				}
				pixel.rgba[3] += weight;
			}
		}
	}
	return OK;
}
} // namespace

namespace gs_hlod {
bool quality_allocation_fits(uint64_t count, uint64_t element_bytes, uint64_t size_max, uint64_t header_bytes)
{
	if (count == 0u) {
		return true; // CowData clears without allocating a header.
	}
	if (count > uint64_t(INT64_MAX) || element_bytes == 0u || count > UINT64_MAX / element_bytes) {
		return false;
	}
	const uint64_t bytes = count * element_bytes;
	// CowData::next_power_of_2 would wrap above 2^63. Round only after this guard.
	if (bytes > (uint64_t(1) << 63u)) {
		return false;
	}
	uint64_t capacity = bytes - 1u;
	capacity |= capacity >> 1u;
	capacity |= capacity >> 2u;
	capacity |= capacity >> 4u;
	capacity |= capacity >> 8u;
	capacity |= capacity >> 16u;
	capacity |= capacity >> 32u;
	++capacity;
	return header_bytes <= size_max && capacity <= size_max - header_bytes;
}
uint64_t quality_allocation_header_bytes()
{
	// Mirror CowData's private SIZE_OFFSET/DATA_OFFSET layout from its actual types.
	// USize is uint64_t even on 32-bit targets; no assumed 8/16-byte constant.
	constexpr uint64_t size_offset =
	        ((sizeof(SafeNumeric<uint64_t>) + alignof(uint64_t) - 1u) / alignof(uint64_t)) * alignof(uint64_t);
	constexpr uint64_t data_offset =
	        ((size_offset + sizeof(uint64_t) + alignof(max_align_t) - 1u) / alignof(max_align_t)) *
	        alignof(max_align_t);
#ifdef DEBUG_ENABLED
	// Memory::alloc_static(..., false) still prepads in a debug-enabled engine.
	return data_offset + Memory::DATA_OFFSET;
#else
	return data_offset;
#endif
}
#ifdef TESTS_ENABLED
static thread_local uint64_t quality_test_size_max = SIZE_MAX;
uint64_t quality_set_test_allocation_size_max(uint64_t size_max)
{
	const uint64_t previous = quality_test_size_max;
	quality_test_size_max = MIN(size_max, uint64_t(SIZE_MAX));
	return previous;
}
#endif
uint64_t quality_allocation_size_max()
{
#ifdef TESTS_ENABLED
	return quality_test_size_max;
#else
	return SIZE_MAX;
#endif
}
QualityView quality_view(uint32_t index)
{
	QualityView view;
	if (index < 6u) {
		view.eye[index / 2u] = (index & 1u) ? -1.0 : 1.0;
	} else if (index < 14u) {
		for (int a = 0; a < 3; ++a) {
			view.eye[a] = ((index - 6u) & (1u << a)) ? -1.0 : 1.0;
		}
		normalize3(view.eye);
	} else {
		const double angle = double(index - 14u) * std::acos(-1.0) / 4.0;
		const double elevation = std::acos(-1.0) / 18.0;
		view.eye[0] = std::cos(elevation) * std::cos(angle);
		view.eye[1] = std::sin(elevation);
		view.eye[2] = std::cos(elevation) * std::sin(angle);
	}
	view.pitch_multiplier = index < 6u ? 1u : 2u;
	int least = 0;
	for (int a = 1; a < 3; ++a) {
		if (std::abs(view.eye[a]) < std::abs(view.eye[least])) {
			least = a;
		}
	}
	double axis[3] = {};
	axis[least] = 1.0;
	cross3(axis, view.eye, view.right);
	normalize3(view.right);
	cross3(view.eye, view.right, view.up);
	return view;
}

Error quality_color(const Gaussian &g, const Vector3 *high, uint32_t first_count, uint32_t high_count,
                    const double ray[3], double rgb[3])
{
	if (first_count > 3u || high_count > 21u || (high_count && !high)) {
		return ERR_INVALID_DATA;
	}
	float basis[25];
	QualitySH::gs_compute_real_sh_basis(Vector3(real_t(ray[0]), real_t(ray[1]), real_t(ray[2])), 4u, basis);
	const double dc[3] = {g.sh_dc.r, g.sh_dc.g, g.sh_dc.b};
	for (int c = 0; c < 3; ++c) {
		if (!std::isfinite(dc[c])) {
			return ERR_INVALID_DATA;
		}
		double value = gaussian_get_dc_encoding(g.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB
		                       ? dc[c] + 0.5
		                       : 1.5 / (1.0 + std::exp(-dc[c])) - 0.25;
		for (uint32_t j = 0; j < first_count; ++j) {
			value += double(g.sh_1[j][c]) * basis[1u + j];
		}
		for (uint32_t j = 0; j < high_count; ++j) {
			value += double(high[j][c]) * basis[4u + j];
		}
		if (!std::isfinite(value)) {
			return ERR_INVALID_DATA;
		}
		rgb[c] = std::clamp(value, 0.0, 1.0);
	}
	return OK;
}
bool quality_prefer_merged(double merged, double selected)
{
	return merged <= 1.01 * selected;
}

Error quality_select(const GaussianSplatHlodTree &tree, const Vector<uint32_t> &leaves, uint32_t first, uint32_t count,
                     const SplatSpan &source, uint32_t wanted, const double center[3], QualityScratch &scratch,
                     QualityPayload &output)
{
	output = QualityPayload();
	if (!count || first > uint64_t(leaves.size()) || count > uint64_t(leaves.size()) - first || !source.gaussians ||
	    (source.sh_high_order_count && !source.sh_high_order)) {
		return ERR_INVALID_DATA;
	}
	if (!gs_hlod::quality_resize_or_fail(scratch.shares, count, "gs_hlod::quality leaf_shares")) {
		return ERR_OUT_OF_MEMORY;
	}
	QualityLeafShare *shares = scratch.shares.ptrw();
	uint64_t total = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t id = leaves[first + i];
		if (id >= tree.nodes.size()) {
			return ERR_INVALID_DATA;
		}
		const GaussianSplatHlodNode &leaf = tree.nodes[id];
		if (leaf.child_count || leaf.payload_first > source.count ||
		    leaf.payload_count > source.count - leaf.payload_first) {
			return ERR_INVALID_DATA;
		}
		shares[i] = {id, leaf.payload_count, 0u, leaf.payload_first, 0u};
		total += leaf.payload_count;
	}
	if (!wanted || wanted > total || total > source.count) {
		return ERR_INVALID_DATA;
	}
	uint64_t assigned = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const uint64_t product = uint64_t(wanted) * shares[i].count; // two uint32 factors
		shares[i].share = uint32_t(product / total);
		shares[i].remainder = product % total;
		assigned += shares[i].share;
	}
	if (count > 1u) {
		std::sort(shares, shares + count, [](const auto &a, const auto &b) {
			return a.remainder > b.remainder || (a.remainder == b.remainder && a.node < b.node);
		});
	}
	const uint64_t remaining = uint64_t(wanted) - assigned;
	if (remaining > count) {
		return ERR_INVALID_DATA;
	}
	for (uint32_t i = 0; i < remaining; ++i) {
		++shares[i].share;
	}
	if (count > 1u) {
		std::sort(shares, shares + count, [](const auto &a, const auto &b) { return a.node < b.node; });
	}
	QualityPayload unsorted;
	unsorted.high_count = source.sh_high_order_count;
	const uint64_t sh_size = uint64_t(wanted) * source.sh_high_order_count;
	if (sh_size > uint64_t(INT64_MAX) / sizeof(Vector3) ||
	    !gs_hlod::quality_resize_or_fail(unsorted.gaussians, wanted, "gs_hlod::quality selected_gaussians") ||
	    (sh_size && !gs_hlod::quality_resize_or_fail(unsorted.sh, int64_t(sh_size), "gs_hlod::quality selected_sh")) ||
	    !gs_hlod::quality_resize_or_fail(scratch.order, wanted, "gs_hlod::quality selected_order")) {
		return ERR_OUT_OF_MEMORY;
	}
	Gaussian *dst = unsorted.gaussians.ptrw();
	Vector3 *dst_sh = unsorted.sh.ptrw();
	QualityOrder *order = scratch.order.ptrw();
	uint32_t cursor = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (shares[i].share > shares[i].count || shares[i].share > wanted - cursor) {
			return ERR_INVALID_DATA;
		}
		for (uint32_t j = 0; j < shares[i].share; ++j) {
			const uint64_t src = shares[i].first + j;
			dst[cursor] = source.gaussians[src];
			for (int a = 0; a < 3; ++a) {
				dst[cursor].position[a] = real_t(double(dst[cursor].position[a]) - center[a]);
				if (!std::isfinite(dst[cursor].position[a])) {
					return ERR_INVALID_DATA;
				}
			}
			if (source.sh_high_order_count) {
				memcpy(dst_sh + uint64_t(cursor) * source.sh_high_order_count,
				       source.sh_high_order + src * source.sh_high_order_count,
				       sizeof(Vector3) * source.sh_high_order_count);
			}
			const float importance = importance_area(dst[cursor]);
			if (!std::isfinite(importance)) {
				return ERR_INVALID_DATA;
			}
			order[cursor] = {importance, cursor};
			++cursor;
		}
	}
	if (cursor != wanted) {
		return ERR_INVALID_DATA;
	}
	if (wanted > 1u) {
		std::sort(order, order + wanted, [](const auto &a, const auto &b) {
			return a.importance > b.importance || (a.importance == b.importance && a.ordinal < b.ordinal);
		});
	}
	QualityPayload sorted;
	sorted.high_count = source.sh_high_order_count;
	if (!gs_hlod::quality_resize_or_fail(sorted.gaussians, wanted, "gs_hlod::quality ordered_gaussians") ||
	    (sh_size && !gs_hlod::quality_resize_or_fail(sorted.sh, int64_t(sh_size), "gs_hlod::quality ordered_sh"))) {
		return ERR_OUT_OF_MEMORY;
	}
	Gaussian *sorted_g = sorted.gaussians.ptrw();
	Vector3 *sorted_sh = sorted.sh.ptrw();
	for (uint32_t i = 0; i < wanted; ++i) {
		const uint32_t src = order[i].ordinal;
		sorted_g[i] = dst[src];
		if (source.sh_high_order_count) {
			memcpy(sorted_sh + uint64_t(i) * source.sh_high_order_count,
			       dst_sh + uint64_t(src) * source.sh_high_order_count, sizeof(Vector3) * source.sh_high_order_count);
		}
	}
	output = std::move(sorted);
	return OK;
}

Error quality_compare_view(const SplatSpan &reference, const SplatSpan &merged, const SplatSpan &selected,
                           uint32_t first, double eps, uint32_t view_index, QualityScratch &scratch,
                           QualityViewResult &result)
{
	result = QualityViewResult();
	if (view_index >= kQualityViewCount || !std::isfinite(eps) || !(eps > 0) || merged.count != selected.count ||
	    reference.sh_high_order_count != merged.sh_high_order_count ||
	    reference.sh_high_order_count != selected.sh_high_order_count) {
		return ERR_INVALID_DATA;
	}
	const QualityView view = quality_view(view_index);
	const double pitch = eps * view.pitch_multiplier;
	if (!std::isfinite(pitch) || !(pitch * pitch > 0) || !std::isfinite(pitch * pitch)) {
		return ERR_INVALID_DATA;
	}
	const SplatSpan spans[3] = {reference, merged, selected};
	const char *labels[3] = {"gs_hlod::quality projected_reference", "gs_hlod::quality projected_merged",
	                         "gs_hlod::quality projected_selected"};
	double lo[2] = {DBL_MAX, DBL_MAX}, hi[2] = {-DBL_MAX, -DBL_MAX};
	bool visible = false;
	for (int candidate = 0; candidate < 3; ++candidate) {
		Error err = project(spans[candidate], first, view, pitch, scratch.projected[candidate], labels[candidate]);
		if (err != OK) {
			return err;
		}
		for (const QualityProjected &p : scratch.projected[candidate]) {
			if (p.alpha < 1.0 / 255.0) {
				continue;
			}
			visible = true;
			lo[0] = MIN(lo[0], p.x - p.rx);
			lo[1] = MIN(lo[1], p.y - p.ry);
			hi[0] = MAX(hi[0], p.x + p.rx);
			hi[1] = MAX(hi[1], p.y + p.ry);
		}
	}
	QualityViewResult domain;
	// An explicitly all-transparent view is one transparent pixel, not missing evidence.
	domain.width = domain.height = 1u;
	if (visible) {
		for (int a = 0; a < 2; ++a) {
			if (!std::isfinite(lo[a]) || !std::isfinite(hi[a]) || std::abs(lo[a]) > double(uint64_t(1) << 50u) ||
			    std::abs(hi[a]) > double(uint64_t(1) << 50u)) {
				return ERR_PARAMETER_RANGE_ERROR;
			}
		}
		domain.x0 = int64_t(std::floor(lo[0]));
		domain.y0 = int64_t(std::floor(lo[1]));
		domain.width = uint64_t(MAX(1.0, std::ceil(hi[0]) - double(domain.x0)));
		domain.height = uint64_t(MAX(1.0, std::ceil(hi[1]) - double(domain.y0)));
	}
	if (domain.height > uint64_t(INT64_MAX) / domain.width ||
	    domain.width * domain.height > uint64_t(SIZE_MAX) / sizeof(QualityPixel) ||
	    domain.width * domain.height > uint64_t(INT64_MAX) / sizeof(QualityPixel)) {
		return ERR_OUT_OF_MEMORY;
	}
	Error err = raster(scratch.projected[0], domain, scratch.reference_image, "gs_hlod::quality reference_image");
	if (err != OK) {
		return err;
	}
	const uint64_t pixels = domain.width * domain.height;
	for (int candidate = 1; candidate < 3; ++candidate) {
		err = raster(scratch.projected[candidate], domain, scratch.candidate_image, "gs_hlod::quality candidate_image");
		if (err != OK) {
			return err;
		}
		double sum = 0;
		for (uint64_t i = 0; i < pixels; ++i) {
			for (int c = 0; c < 4; ++c) {
				const double difference = scratch.candidate_image[i].rgba[c] - scratch.reference_image[i].rgba[c];
				sum += difference * difference;
			}
		}
		const double error = sum / (4.0 * double(pixels));
		if (!std::isfinite(error)) {
			return ERR_INVALID_DATA;
		}
		if (candidate == 1) {
			domain.merged_mse = error;
		} else {
			domain.selected_mse = error;
		}
	}
	result = domain;
	return OK;
}
Error quality_compare(const SplatSpan &reference, const SplatSpan &merged, const SplatSpan &selected, uint32_t first,
                      double eps, QualityScratch &scratch, QualityResult &result)
{
	QualityResult candidate;
	double merged_sum = 0, selected_sum = 0;
	for (uint32_t view = 0; view < kQualityViewCount; ++view) {
		const Error err =
		        quality_compare_view(reference, merged, selected, first, eps, view, scratch, candidate.views[view]);
		if (err != OK) {
			return err;
		}
		merged_sum += candidate.views[view].merged_mse;
		selected_sum += candidate.views[view].selected_mse;
	}
	const double m = merged_sum / kQualityViewCount, s = selected_sum / kQualityViewCount;
	candidate.merged_error = float(m);
	candidate.selected_error = float(s);
	if (!std::isfinite(candidate.merged_error) || !std::isfinite(candidate.selected_error)) {
		return ERR_INVALID_DATA;
	}
	candidate.use_merged = quality_prefer_merged(m, s);
	result = candidate;
	return OK;
}
} // namespace gs_hlod
