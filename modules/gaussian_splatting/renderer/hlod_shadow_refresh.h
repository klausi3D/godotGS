#ifndef GS_HLOD_SHADOW_REFRESH_H
#define GS_HLOD_SHADOW_REFRESH_H

#include "hlod_shadow_caster_plan.h"
#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include <cstring>
#include <limits>

namespace gs_shadow_refresh {

// Render-thread CPU state only. No GPU ownership, worker wait, or additional lock.
// FNV-1a 64-bit digest: accidental collisions remain possible (~2^-64 per independent comparison).
// Hash once per record/row, then combine only spatial hits per light; retaining exact bytes per light would
// duplicate the intersecting draw/row data for every lamp. This is a cache key, not an adversarial hash.
inline uint64_t hash_bytes(uint64_t p_hash, const void *p_data, size_t p_size) {
	const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
	for (size_t i = 0; i < p_size; ++i) { p_hash = (p_hash ^ bytes[i]) * UINT64_C(1099511628211); }
	return p_hash;
}
static constexpr uint64_t HASH_SEED = UINT64_C(14695981039346656037);

template <typename T>
inline uint64_t hash_value(uint64_t p_hash, const T &p_value) {
	return hash_bytes(p_hash, &p_value, sizeof(T));
}

// Preserve both endpoints when the float span crosses a large origin (e.g. [-2e8, 1]). AABB(lo, hi-lo)
// alone can round its computed end back to zero. Aggregate cells must not narrow and prune an edge caster.
inline AABB enclosing_bounds(const Vector3 &p_lower, const Vector3 &p_upper) {
	Vector3 size;
	for (int axis = 0; axis < 3; ++axis) {
		const double span = double(p_upper[axis]) - double(p_lower[axis]);
		size[axis] = real_t(span);
		if (double(size[axis]) < span || p_lower[axis] + size[axis] < p_upper[axis]) {
			size[axis] = std::nextafter(size[axis], std::numeric_limits<real_t>::infinity());
		}
	}
	return AABB(p_lower, size);
}

inline AABB enclosing_merge(const AABB &p_a, const AABB &p_b) {
	return enclosing_bounds(p_a.position.min(p_b.position),
			(p_a.position + p_a.size).max(p_b.position + p_b.size));
}

struct Geometry {
	AABB bounds; // World node: world-space bounds. Object chunk: asset-local bounds.
	uint64_t payload = 0; // Per-node content version / per-chunk payload digest, never the whole cut's revision.
	uint32_t chunk_key = 0; // World node slot or asset-local object chunk, never a dense bundle table address.
	// Caster support for per-view record lists (hlod_shadow_face_cull.h); never part of the refresh digest.
	// 0 = unknown (the record is never culled). 1 = world node: `bounds` contains every payload splat's three-sigma
	// support (loader-validated). 2 = object chunk: the sphere of radius `support_radius` about bounds' centre does.
	uint8_t support_kind = 0;
	real_t support_radius = -1;
};

struct PublishedContent {
	LocalVector<DrawnNodeGPU> draws;
	// The uploaded list byte for byte (draws[] above zeroes the offsets), so a per-view subset can be uploaded instead.
	LocalVector<DrawnNodeGPU> gpu_draws;
	LocalVector<Geometry> geometry;
	void publish(const gs_shadow_caster::FrameRecord &p_record, const DrawnNodeGPU *p_draws,
			const LocalVector<Geometry> &p_geometry) {
		draws.resize(p_record.draw_count);
		gpu_draws.resize(p_record.draw_count);
		for (uint32_t i = 0; i < p_record.draw_count; ++i) {
			gpu_draws[i] = p_draws[i];
			draws[i] = p_draws[i];
			// frame_relative_shadow_clip compensates FrameRecord::camera. World identity comes from Geometry,
			// not from adding an anchor to rounded GPU offsets; object translation comes from current rows.
			for (float &offset : draws[i].camera_offset) { offset = 0; }
		}
		geometry = p_geometry;
	}
};

inline AABB record_bounds(const Geometry &p_geometry, uint32_t p_row, const InstanceDataGPU &p_data, real_t p_margin) {
	AABB bounds = p_geometry.bounds;
	if (p_row != 0) {
		const Basis basis = Basis(Quaternion(p_data.rotation[0], p_data.rotation[1], p_data.rotation[2], p_data.rotation[3]))
				.scaled(Vector3(p_data.translation_scale[3], p_data.translation_scale[3], p_data.translation_scale[3]));
		bounds = Transform3D(basis, Vector3(p_data.translation_scale[0], p_data.translation_scale[1], p_data.translation_scale[2])).xform(bounds);
	}
	bounds.grow_by(p_margin);
	return bounds;
}

struct SpatialRecord {
	AABB bounds;
	uint64_t digest = 0;
	bool animated = false;
	uint32_t next = UINT32_MAX;
};

// Fixed-depth sparse octree of record centres, with aggregate *full inflated bounds* at every node (large records
// cannot be lost by centre assignment). O(6*N) build, <=min(1+6*N, 299593) nodes, O(N) storage. Query prunes
// disjoint cells and consumes fully enclosed cells in O(1), including their digest and animation flag. Only boundary
// cells visit records; cost is O(N + sum_light(visited cells + boundary-leaf candidates)), not an unconditional
// lights*records scan. Degenerate boundary clustering can still visit N records per light. Depth is fixed at six.
struct ContentIndex {
	struct Cell {
		AABB bounds;
		uint32_t children[8] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
		uint32_t head = UINT32_MAX;
		uint64_t digest = 0;
		uint32_t count = 0;
		bool animated = false;
		void add(const SpatialRecord &p_record) {
			if (count == 0) { bounds = p_record.bounds; } else { bounds = enclosing_merge(bounds, p_record.bounds); }
			digest += p_record.digest;
			++count;
			animated |= p_record.animated;
		}
	};
	LocalVector<SpatialRecord> records;
	LocalVector<Cell> cells;
	void build() {
		cells.clear();
		if (records.is_empty()) { return; }
		Cell root;
		for (const SpatialRecord &record : records) { root.add(record); }
		cells.push_back(root);
		for (uint32_t i = 0; i < records.size(); ++i) {
			Vector3 lo = root.bounds.position, size = root.bounds.size;
			const Vector3 center = records[i].bounds.get_center();
			uint32_t cell = 0;
			for (uint32_t depth = 0; depth < 6; ++depth) {
				size *= real_t(0.5);
				uint32_t octant = 0;
				for (int axis = 0; axis < 3; ++axis) {
					if (center[axis] >= lo[axis] + size[axis]) { octant |= 1u << axis; lo[axis] += size[axis]; }
				}
				uint32_t child = cells[cell].children[octant];
				if (child == UINT32_MAX) {
					child = cells.size();
					cells[cell].children[octant] = child;
					Cell added;
					cells.push_back(added);
				}
				cells[child].add(records[i]);
				cell = child;
			}
			records[i].next = cells[cell].head;
			cells[cell].head = i;
		}
	}
	void rebuild(const PublishedContent &p_content, const gs_shadow_caster::FrameRecord &p_record,
			const LocalVector<InstanceDataGPU> &p_rows, const LocalVector<real_t> &p_margins,
			const LocalVector<uint32_t> &p_animated);
	void query(uint32_t p_cell, const Vector3 &p_center, real_t p_radius,
			uint64_t &r_digest, uint32_t &r_count, bool &r_animated) const;
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

inline bool sphere_contains_bounds(const Vector3 &p_center, real_t p_radius, const AABB &p_bounds) {
	const Vector3 end = p_bounds.position + p_bounds.size;
	const real_t dx = MAX(Math::abs(p_bounds.position.x - p_center.x), Math::abs(end.x - p_center.x));
	const real_t dy = MAX(Math::abs(p_bounds.position.y - p_center.y), Math::abs(end.y - p_center.y));
	const real_t dz = MAX(Math::abs(p_bounds.position.z - p_center.z), Math::abs(end.z - p_center.z));
	return p_radius >= 0 && dx * dx + dy * dy + dz * dz <= p_radius * p_radius;
}

inline void ContentIndex::query(uint32_t p_cell, const Vector3 &p_center, real_t p_radius,
		uint64_t &r_digest, uint32_t &r_count, bool &r_animated) const {
	const Cell &cell = cells[p_cell];
	if (!sphere_intersects_bounds(p_center, p_radius, cell.bounds)) { return; }
	if (sphere_contains_bounds(p_center, p_radius, cell.bounds)) {
		r_digest += cell.digest;
		r_count += cell.count;
		r_animated |= cell.animated;
		return;
	}
	for (uint32_t i = cell.head; i != UINT32_MAX; i = records[i].next) {
		const SpatialRecord &record = records[i];
		if (!sphere_intersects_bounds(p_center, p_radius, record.bounds)) { continue; }
		// Commutative multiset digest: cut/list order and spatial-cell changes cannot dirty a map.
		r_digest += record.digest;
		++r_count;
		r_animated |= record.animated;
	}
	for (uint32_t child : cell.children) {
		if (child != UINT32_MAX) { query(child, p_center, p_radius, r_digest, r_count, r_animated); }
	}
}

// No camera, list upload/build counter, whole-cut revision, or dense row index in the content identity.
inline uint64_t record_digest(const DrawnNodeGPU &p_draw, const Geometry &p_geometry, uint64_t p_row_digest) {
	uint64_t digest = hash_value(p_row_digest, p_geometry.chunk_key);
	// The caster draws a record iff fade >= GS_HLOD_SHADOW_CASTER_MIN_FADE (S6); only that predicate changes the map, so a
	// cross-fade that moves every frame does not re-render shadows every frame.
	const uint8_t casts = p_draw.fade >= GS_HLOD_SHADOW_CASTER_MIN_FADE ? 1u : 0u;
	digest = hash_value(digest, casts);
	digest = hash_value(digest, p_draw.level);
	digest = hash_value(digest, p_geometry.payload);
	for (int axis = 0; axis < 3; ++axis) {
		digest = hash_value(digest, p_geometry.bounds.position[axis]);
		digest = hash_value(digest, p_geometry.bounds.size[axis]);
	}
	// Avalanche before commutative addition, so neighbouring node/row keys do not have linear sums.
	digest = (digest ^ (digest >> 33)) * UINT64_C(0xff51afd7ed558ccd);
	digest = (digest ^ (digest >> 33)) * UINT64_C(0xc4ceb9fe1a85ec53);
	return digest ^ (digest >> 33);
}

inline uint64_t row_digest(const InstanceDataGPU &p_row, uint64_t p_key, uint64_t p_asset) {
	InstanceDataGPU row = p_row; // Layout has asserted size and no padding.
	row.ids[0] = 0; // The stable asset identity below replaces the bundle's dense remap.
	row.lod[0] = 0; // Ignore camera-selected LOD; lod.y remains the semantic override base.
	row.params[1] = 0; // LOD bias affects admission, already represented by selected node/chunk identities.
	return hash_value(hash_value(hash_value(HASH_SEED, p_key), p_asset), row);
}

inline void ContentIndex::rebuild(const PublishedContent &p_content, const gs_shadow_caster::FrameRecord &p_record,
		const LocalVector<InstanceDataGPU> &p_rows, const LocalVector<real_t> &p_margins,
		const LocalVector<uint32_t> &p_animated) {
	LocalVector<uint64_t> row_digests;
	row_digests.resize(p_rows.size());
	for (uint32_t row = 0; row < p_rows.size(); ++row) {
		row_digests[row] = row_digest(p_rows[row], p_record.row_keys[row], p_record.row_asset_ids[row]);
	}
	records.clear();
	for (uint32_t i = 0; i < p_content.draws.size(); ++i) {
		const auto &draw = p_content.draws[i];
		const uint32_t row = draw.instance_id;
		if (row >= p_rows.size() || !p_record.row_casts[row]) { continue; }
		SpatialRecord spatial;
		spatial.bounds = record_bounds(p_content.geometry[i], row, p_rows[row], p_margins[row]);
		spatial.digest = record_digest(draw, p_content.geometry[i], row_digests[row]);
		spatial.animated = p_animated[row] != 0;
		records.push_back(spatial);
	}
	build();
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
	uint32_t record_count = 0;
	const ContentIndex *content = nullptr; // Borrowed only while preparing this view; never retained by LightState.

	bool same_content(const Owner &p_other) const {
		return owner == p_other.owner && renderer == p_other.renderer && revision == p_other.revision &&
				opacity == p_other.opacity && record_count == p_other.record_count && animated == p_other.animated;
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
	void commit(const LightUpdate &p_update) {
		last_pass = p_update.owners;
		for (Owner &owner : last_pass) { owner.content = nullptr; }
	}
};

inline LightUpdate prepare_light(const LocalVector<Owner> &p_owners, const Vector3 &p_center, real_t p_radius,
		const LightState &p_state) {
	LightUpdate update;
	for (const Owner &source : p_owners) {
		if (!sphere_intersects_bounds(p_center, p_radius, source.bounds)) { continue; }
		Owner owner = source;
		if (source.content) {
			owner.revision = 0;
			owner.record_count = 0;
			owner.animated = false;
			if (!source.content->cells.is_empty()) {
				source.content->query(0, p_center, p_radius, owner.revision, owner.record_count, owner.animated);
			}
			if (owner.record_count == 0) { continue; }
		}
		owner.content = nullptr;
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
