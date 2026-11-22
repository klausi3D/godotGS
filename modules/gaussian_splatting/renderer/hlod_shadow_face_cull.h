#ifndef GS_HLOD_SHADOW_FACE_CULL_H
#define GS_HLOD_SHADOW_FACE_CULL_H

// Per light view (directional cascade, spot, omni cube face) subset of the HLOD caster's draw list (shadow split ADR
// addendum, PR0). Device-free; shared by the renderer and the CPU doctests.
//
// The caster (shaders/gs_shadow_caster.glsl) issues draw_count * max_chunk_splats instanced quads for EVERY view and rejects
// off-view splats one vertex at a time. This drops a record from a view only when no splat of it can pass the shader's
// own clip tests, so the depth the view receives is bit-identical to drawing the whole list: the hardware depth test
// keeps the maximum (reverse-Z) of every surviving fragment, independent of draw order.
//
// Why a dropped record has no surviving splat. Each record's world-space box contains the three-sigma support of every
// splat it can emit, displaced by at most the row's wind/effector bound:
//   - world nodes: the node AABB contains its payload's three-sigma support (validated when the world is loaded,
//     io/gaussian_splat_world_paged.cpp, gaussian_splat_world_io.cpp; edits stay inside, lod/hlod_edit.h), and the
//     world row must be identity-placed (otherwise the record is kept);
//   - object chunks: a sphere about the chunk centre of radius max(|p - c| + 3 * max(scale)) over its payload, placed
//     by the row's translation with max(1, |uniform scale|) (both flag branches of gs_hlod_camera_relative_position).
// The shader keeps a quad only if |ndc.xy| <= 1 + extent_ndc, extent = 1.177 sigma (alpha threshold 0.5) along the
// footprint's eigen axes plus the 0.3 texel^2 low-pass. Per screen axis that reach is <= sqrt(2) * 1.177 sigma +
// 1.3 texels, inside the projected three-sigma reach plus the guard band below. A box is dropped only when it lies
// entirely in one outer half-space of the view (x or y beyond 1 + guard, beyond the far plane, or behind a perspective
// light); the near plane is never used (directional casters in front of it are pancaked, not clipped).

#include "hlod_shadow_refresh.h"

#include "core/math/aabb.h"
#include "core/math/projection.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace gs_shadow_face_cull {

// Guard band per side, in texels of the view rect. Covers the low-pass dilation (<= 1.3 texels), float rounding of
// camera-relative positions and the perspective linearisation of the quad.
static constexpr double kGuardTexels = 4.0;
// Support radius in sigmas that the box must contain (the caster's quad reaches at most 1.177 sigma per eigen axis).
static constexpr double kSupportSigmas = 3.0;

enum SupportKind : uint8_t {
	SUPPORT_UNKNOWN = 0,
	SUPPORT_WORLD_NODE_AABB = 1,
	SUPPORT_LOCAL_SPHERE = 2,
};

// Asset-local radius about p_center that contains the three-sigma support of every splat; false (record never culled)
// for an empty or non-finite payload. T has float position[3] and scale[3] (PackedGaussian).
template <typename T>
inline bool local_support_radius(const T *p_splats, uint32_t p_count, const Vector3 &p_center, real_t &r_radius) {
	if (!p_splats || p_count == 0) { return false; }
	double radius = 0.0;
	for (uint32_t i = 0; i < p_count; ++i) {
		const T &g = p_splats[i];
		double distance_sq = 0.0, max_scale = 0.0;
		for (int a = 0; a < 3; ++a) {
			const double d = double(g.position[a]) - double(p_center[a]);
			distance_sq += d * d;
			max_scale = std::fmax(max_scale, std::fabs(double(g.scale[a])));
		}
		const double r = std::sqrt(distance_sq) + kSupportSigmas * max_scale;
		if (!std::isfinite(r)) { return false; }
		radius = std::fmax(radius, r);
	}
	real_t rounded = real_t(radius);
	if (double(rounded) < radius) { rounded = std::nextafter(rounded, std::numeric_limits<real_t>::infinity()); }
	if (!std::isfinite(double(rounded))) { return false; }
	r_radius = rounded;
	return true;
}

// World-space caster support of one record, or false when it cannot be bounded (the record is then always drawn).
inline bool record_support(const gs_shadow_refresh::Geometry &p_geometry, uint32_t p_row, const InstanceDataGPU &p_data,
		real_t p_margin, AABB &r_box) {
	if (!(p_margin >= 0) || !std::isfinite(double(p_margin))) { return false; }
	if (p_row == 0) {
		// World records: offsets are the node centres of an identity-placed world (resident_instance_contract_publisher:
		// make_drawn_node with WORLD_ROW). Any other placement keeps every world record.
		const uint32_t identity = GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO;
		if (p_geometry.support_kind != SUPPORT_WORLD_NODE_AABB || (p_data.ids[1] & identity) != identity) { return false; }
		r_box = p_geometry.bounds;
		r_box.grow_by(p_margin);
		return true;
	}
	if (p_geometry.support_kind != SUPPORT_LOCAL_SPHERE || !(p_geometry.support_radius >= 0)) { return false; }
	const Vector3 center = p_geometry.bounds.get_center();
	const double scale = std::fmax(1.0, std::fabs(double(p_data.translation_scale[3])));
	const double radius = scale * (double(center.length()) + double(p_geometry.support_radius)) + double(p_margin);
	if (!std::isfinite(radius)) { return false; }
	const Vector3 origin(p_data.translation_scale[0], p_data.translation_scale[1], p_data.translation_scale[2]);
	if (!std::isfinite(double(origin.x)) || !std::isfinite(double(origin.y)) || !std::isfinite(double(origin.z))) { return false; }
	real_t r = real_t(radius);
	if (double(r) < radius) { r = std::nextafter(r, std::numeric_limits<real_t>::infinity()); }
	r_box = gs_shadow_refresh::enclosing_bounds(origin - Vector3(r, r, r), origin + Vector3(r, r, r));
	r_box.grow_by(real_t(std::fmax(std::fabs(double(origin.x)), std::fmax(std::fabs(double(origin.y)), std::fabs(double(origin.z)))) * 1e-6));
	return true;
}

// The per-frame CPU snapshot one renderer's light views cull against. Render thread only; rebuilt with the per-light
// content index (GaussianSplatRenderer::collect_hlod_shadow_refresh_owners), valid only for the exact draw list and
// RenderingServer frame it was built for.
struct Snapshot {
	bool valid = false;
	uint64_t bundle_builds = 0;
	uint64_t draw_list_upload = 0;
	uint64_t server_frame = UINT64_MAX;
	uint32_t draw_count = 0;
	LocalVector<uint32_t> rows; // DrawnNodeGPU::instance_id per record.
	LocalVector<AABB> boxes; // World-space caster support per record.
	LocalVector<uint8_t> known; // 0: unbounded, never culled.

	void clear() {
		valid = false;
		draw_count = 0;
		rows.clear();
		boxes.clear();
		known.clear();
	}

	void build(const gs_shadow_caster::FrameRecord &p_record, const gs_shadow_refresh::PublishedContent &p_content,
			const LocalVector<InstanceDataGPU> &p_rows, const LocalVector<real_t> &p_margins, uint64_t p_server_frame) {
		clear();
		if (!p_record.valid || p_content.gpu_draws.size() != p_record.draw_count ||
				p_content.geometry.size() != p_record.draw_count || p_margins.size() != p_rows.size()) {
			return;
		}
		rows.resize(p_record.draw_count);
		boxes.resize(p_record.draw_count);
		known.resize(p_record.draw_count);
		for (uint32_t i = 0; i < p_record.draw_count; ++i) {
			const uint32_t row = p_content.gpu_draws[i].instance_id;
			rows[i] = row;
			known[i] = row < p_rows.size() && record_support(p_content.geometry[i], row, p_rows[row], p_margins[row], boxes[i]);
		}
		bundle_builds = p_record.bundle_builds;
		draw_list_upload = p_record.draw_list_upload;
		draw_count = p_record.draw_count;
		server_frame = p_server_frame;
		valid = true;
	}

	bool matches(const gs_shadow_caster::FrameRecord &p_record, uint64_t p_server_frame) const {
		return valid && p_record.valid && server_frame == p_server_frame && bundle_builds == p_record.bundle_builds &&
				draw_list_upload == p_record.draw_list_upload && draw_count == p_record.draw_count;
	}
};

// One light view: the caster's clip transform of frame-relative positions (frame_relative_shadow_clip) and its rect.
struct View {
	double rows[4][4] = {}; // rows[r][c]: clip_r = sum_c rows[r][c] * (p - frame_camera, 1)_c
	double camera[3] = {};
	double guard_x = 0.0;
	double guard_y = 0.0;
	bool perspective = false;
	bool valid = false;

	View() = default;
	View(const Projection &p_clip_from_frame, const Vector3 &p_frame_camera, const Vector2i &p_rect_size, bool p_perspective,
			double p_guard_texels = kGuardTexels) {
		for (int r = 0; r < 4; ++r) {
			for (int c = 0; c < 4; ++c) {
				rows[r][c] = double(p_clip_from_frame.columns[c][r]);
				valid = valid || rows[r][c] != 0.0;
			}
		}
		for (int a = 0; a < 3; ++a) { camera[a] = double(p_frame_camera[a]); }
		perspective = p_perspective;
		valid = valid && p_rect_size.x > 0 && p_rect_size.y > 0;
		if (valid) {
			guard_x = 2.0 * p_guard_texels / double(p_rect_size.x);
			guard_y = 2.0 * p_guard_texels / double(p_rect_size.y);
		}
		for (int r = 0; r < 4 && valid; ++r) {
			for (int c = 0; c < 4; ++c) { valid = valid && std::isfinite(rows[r][c]); }
		}
	}

	// Minimum and maximum over the box of the affine form row . (p - camera, 1).
	void range(const double p_row[4], const double p_center[3], const double p_half[3], double &r_min, double &r_max) const {
		double mid = p_row[3], spread = 0.0;
		for (int a = 0; a < 3; ++a) {
			mid += p_row[a] * p_center[a];
			spread += std::fabs(p_row[a]) * p_half[a];
		}
		r_min = mid - spread;
		r_max = mid + spread;
	}

	// True only when every point of p_box is rejected by the caster's clip tests (see the file comment).
	bool box_outside(const AABB &p_box) const {
		if (!valid) { return false; }
		double center[3], half[3];
		for (int a = 0; a < 3; ++a) {
			half[a] = 0.5 * double(p_box.size[a]);
			center[a] = double(p_box.position[a]) + half[a] - camera[a];
			if (!std::isfinite(center[a]) || !std::isfinite(half[a]) || half[a] < 0.0) { return false; }
		}
		double w_min, w_max;
		range(rows[3], center, half, w_min, w_max);
		if (perspective && w_max < 0.0) { return true; } // Behind the light: every centre has clip.w <= 1e-6.
		const double w_abs = std::fmax(std::fabs(w_min), std::fabs(w_max));
		for (int axis = 0; axis < 2; ++axis) {
			const double guard = 1.0 + (axis == 0 ? guard_x : guard_y);
			double outer[4], inner[4];
			for (int c = 0; c < 4; ++c) {
				outer[c] = rows[axis][c] - guard * rows[3][c]; // x - (1 + g) w > 0
				inner[c] = -rows[axis][c] - guard * rows[3][c]; // -x - (1 + g) w > 0
			}
			double lo, hi;
			range(outer, center, half, lo, hi);
			if (lo > 1e-9 * (1.0 + w_abs)) { return true; }
			range(inner, center, half, lo, hi);
			if (lo > 1e-9 * (1.0 + w_abs)) { return true; }
		}
		// Beyond the far plane (ndc.z < 0 with w > 0). The margin dwarfs float rounding of clip.z.
		double z_min, z_max;
		range(rows[2], center, half, z_min, z_max);
		if (z_max < -1e-4 * std::fmax(w_abs, 1e-3)) { return true; }
		return false;
	}
};

struct ListStats {
	uint32_t total = 0;
	uint32_t listed = 0;
	uint32_t culled_outside = 0;
	uint32_t culled_noncasting = 0;
	uint32_t unbounded = 0;
	bool placeholder = false; // Nothing survived; one rejected record keeps the pass's side effects (no fragment).
};

// The records of p_snapshot that the view can receive, in list order, as indices into the published list. A record
// whose row does not cast is skipped exactly as the shader skips it (row_casts[row] == 0 for an uploaded row).
// p_unsafe_centre_only is a TEST-ONLY negative control: it culls by each box's centre point, which drops records that
// still reach the view, so a depth comparison against the whole list must see the difference.
inline void build_view_list(const Snapshot &p_snapshot, const LocalVector<uint32_t> &p_row_casts, const View &p_view,
		LocalVector<uint32_t> &r_list, ListStats &r_stats, bool p_unsafe_centre_only = false) {
	r_list.clear();
	r_stats = ListStats();
	r_stats.total = p_snapshot.draw_count;
	for (uint32_t i = 0; i < p_snapshot.draw_count; ++i) {
		const uint32_t row = p_snapshot.rows[i];
		if (row < p_row_casts.size() && p_row_casts[row] == 0) {
			++r_stats.culled_noncasting;
			continue;
		}
		if (!p_snapshot.known[i]) {
			++r_stats.unbounded;
		} else if (p_view.box_outside(p_unsafe_centre_only ? AABB(p_snapshot.boxes[i].get_center(), Vector3()) : p_snapshot.boxes[i])) {
			++r_stats.culled_outside;
			continue;
		}
		r_list.push_back(i);
	}
	if (r_list.is_empty() && p_snapshot.draw_count > 0) {
		r_list.push_back(0); // Proven to emit nothing into this view.
		r_stats.placeholder = true;
	}
	r_stats.listed = r_list.size();
}

} // namespace gs_shadow_face_cull

#endif // GS_HLOD_SHADOW_FACE_CULL_H
