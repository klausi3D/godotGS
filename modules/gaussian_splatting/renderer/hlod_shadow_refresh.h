#ifndef GS_HLOD_SHADOW_REFRESH_H
#define GS_HLOD_SHADOW_REFRESH_H

#include "hlod_shadow_caster_plan.h"
#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include <cstring>

namespace gs_shadow_refresh {

// Uploads happen even for identical lists. Keep one exact CPU snapshot per renderer, not a hash or an upload counter.
// All fields are render-thread-only. No GPU readback, worker wait, or additional lock is needed.
struct PublishedContent {
	gs_shadow_caster::FrameRecord previous;
	LocalVector<DrawnNodeGPU> draws;
	LocalVector<InstanceDataGPU> rows;
	uint64_t source_revision = 0;
	uint64_t revision = 0;

	template <typename T>
	static bool equal_bytes(const LocalVector<T> &p_old, const T *p_new, uint32_t p_count) {
		return p_old.size() == p_count && (p_count == 0 || std::memcmp(p_old.ptr(), p_new, sizeof(T) * p_count) == 0);
	}
	static LocalVector<InstanceDataGPU> casting_rows(const gs_shadow_caster::FrameRecord &p_record,
			const LocalVector<InstanceDataGPU> &p_rows) {
		LocalVector<InstanceDataGPU> result;
		for (uint32_t row = 0; row < p_rows.size() && row < p_record.row_casts.size(); ++row) {
			if (p_record.row_casts[row]) { result.push_back(p_rows[row]); }
		}
		return result;
	}

	// An end-of-main-view idle restore may upload newer rows without replacing the retained draw list.
	uint64_t refresh_rows(const gs_shadow_caster::FrameRecord &p_record, const LocalVector<InstanceDataGPU> &p_rows) {
		const auto current = casting_rows(p_record, p_rows);
		if (!equal_bytes(rows, current.ptr(), current.size()) ||
				!equal_bytes(previous.row_casts, p_record.row_casts.ptr(), p_record.row_casts.size())) {
			rows = current;
			previous.row_casts = p_record.row_casts;
			++revision;
		}
		return revision;
	}

	uint64_t publish(const gs_shadow_caster::FrameRecord &p_record, const DrawnNodeGPU *p_draws,
			const LocalVector<InstanceDataGPU> &p_rows, uint64_t p_source_revision) {
		LocalVector<DrawnNodeGPU> casting_draws;
		for (uint32_t i = 0; i < p_record.draw_count; ++i) {
			const uint32_t row = p_draws[i].instance_id;
			if (row < p_record.row_casts.size() && p_record.row_casts[row]) { casting_draws.push_back(p_draws[i]); }
		}
		const auto current_rows = casting_rows(p_record, p_rows);
		// Both GPU layouts have asserted sizes and no padding. Ignore renderer_frame and draw_list_upload: neither
		// identifies a content change. Camera-relative offsets, row transforms, cast flags, edits and rebuilds do.
		const bool same = revision != 0 && previous.bundle_builds == p_record.bundle_builds &&
				previous.origin_revision == p_record.origin_revision && previous.camera == p_record.camera &&
				source_revision == p_source_revision &&
				equal_bytes(draws, casting_draws.ptr(), casting_draws.size()) &&
				equal_bytes(rows, current_rows.ptr(), current_rows.size()) &&
				equal_bytes(previous.row_casts, p_record.row_casts.ptr(), p_record.row_casts.size());
		if (!same) {
			++revision;
			draws = casting_draws;
			rows = current_rows;
			previous = p_record;
			source_revision = p_source_revision;
		}
		return revision;
	}
};

// Godot's spot shadow projection has axial length range and base radius range*tan(angle). The apex-centred sphere
// with radius range/cos(angle) encloses that cone, including its far rim. Omnis retain their exact range sphere.
inline real_t positional_sphere_radius(real_t p_range, real_t p_spot_angle) {
	return p_spot_angle < 0 ? p_range : p_range / MAX(Math::cos(Math::deg_to_rad(p_spot_angle)), real_t(1e-6));
}

// Includes touching boundaries and avoids the much looser light-AABB test for omnis.
inline bool sphere_intersects_bounds(const Vector3 &p_center, real_t p_radius, const AABB &p_bounds) {
	const Vector3 end = p_bounds.position + p_bounds.size;
	const real_t dx = MAX(MAX(p_bounds.position.x - p_center.x, p_center.x - end.x), real_t(0));
	const real_t dy = MAX(MAX(p_bounds.position.y - p_center.y, p_center.y - end.y), real_t(0));
	const real_t dz = MAX(MAX(p_bounds.position.z - p_center.z, p_center.z - end.z), real_t(0));
	return p_radius >= 0 && dx * dx + dy * dy + dz * dz <= p_radius * p_radius;
}

inline bool needs_refresh(bool p_intersects, bool p_seen, bool p_changed, bool p_animated) {
	return p_intersects && (!p_seen || p_changed || p_animated);
}

inline bool effectors_active_on_casting_row(const InstanceDataGPU &p_row, bool p_casts, const float (&p_meta)[4],
		const float (&p_spheres)[GS_MAX_SPHERE_EFFECTORS][4], const float (&p_configs)[GS_MAX_SPHERE_EFFECTORS][4],
		const float (&p_opacity)[GS_MAX_SPHERE_EFFECTORS][4]) {
	if (!p_casts) { return false; }
	uint32_t mask = 0;
	std::memcpy(&mask, &p_row.effect_params[2], sizeof(mask));
	for (uint32_t slot = 0; slot < MIN(uint32_t(p_meta[0]), GS_MAX_SPHERE_EFFECTORS); ++slot) {
		// Conservatively ignore sphere/splat overlap, time, and per-node fade. Scene masks retain packed slot indices;
		// the legacy project sphere applies without a scene mask. Both position and opacity can change shadow content.
		if (p_configs[slot][0] <= 0.5f || p_spheres[slot][3] <= 1e-6f ||
				(p_meta[3] > 0.5f && (mask & (1u << slot)) == 0)) { continue; }
		if ((p_row.effect_params[0] > 0.0f && p_opacity[slot][0] > 0.5f && std::abs(p_configs[slot][1]) > 1e-8f) ||
				(p_row.effect_params[1] > 0.0f && p_opacity[slot][1] > 0.5f && p_opacity[slot][2] > 0.0f)) { return true; }
	}
	return false;
}

struct Owner {
	uint64_t owner = 0;
	uint64_t renderer = 0;
	uint64_t revision = 0;
	float opacity = 1.0f;
	AABB bounds;
	bool animated = false;

	bool same_content(const Owner &p_other) const {
		return owner == p_other.owner && renderer == p_other.renderer && revision == p_other.revision &&
				opacity == p_other.opacity && bounds == p_other.bounds &&
				animated == p_other.animated;
	}
};

struct LightUpdate {
	LocalVector<Owner> owners;
	bool refresh = false;
};

// The module's render-thread cache holds one state per live light, keyed by light-instance RID. Its last-pass list is
// bounded by the intersecting visible owners of that pass; the scene-cull light-free hook erases the whole entry.
struct LightState {
	LocalVector<Owner> last_pass;
	void commit(const LightUpdate &p_update) { last_pass = p_update.owners; }
};

inline LightUpdate prepare_light(const LocalVector<Owner> &p_owners, const Vector3 &p_center, real_t p_radius,
		const LightState &p_state) {
	LightUpdate update;
	for (const Owner &owner : p_owners) {
		if (!sphere_intersects_bounds(p_center, p_radius, owner.bounds)) { continue; }
		const Owner *seen = nullptr;
		for (const Owner &old : p_state.last_pass) {
			if (old.owner == owner.owner) { seen = &old; break; }
		}
		update.refresh |= needs_refresh(true, seen != nullptr, seen && !owner.same_content(*seen), owner.animated);
		update.owners.push_back(owner);
	}
	// A departed owner must be cleared even when it was the last one. Committing that empty pass ends the refresh.
	// Equal-size replacements already refresh through the unseen-owner check above; two empty sets never refresh.
	if (update.owners.size() != p_state.last_pass.size()) { update.refresh = true; }
	return update;
}

} // namespace gs_shadow_refresh
#endif
