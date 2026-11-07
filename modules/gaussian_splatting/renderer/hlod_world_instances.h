#ifndef GS_HLOD_WORLD_INSTANCES_H
#define GS_HLOD_WORLD_INSTANCES_H

#include "../core/gaussian_splat_world_frame.h"
#include "../lod/hlod_runtime_selection.h"
#include "core/math/plane.h"
#include "core/math/vector3.h"
#include "hlod_bundle_rows.h"
#include "hlod_drawn_node.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// PR1b of "world + movable instances in one frame" (docs/architecture/adr-one-route-many-producers.md, sections 4.1 to
// 4.5): the HLOD route draws the world (row 0) and the visible GaussianSplatNode3D instances (rows 1..N) in one draw list,
// one depth sort and one byte ledger. This header is the device-free part, tested on the CPU:
//
// - the instance region of a bundle: every distinct node asset is packed once, after the world, as whole chunks
//   (atlas records [world_records, +records), chunk tables [node_count, +chunk_count)); instances of one asset share it;
// - the ledger and the capacities of a bundle with that region (for an empty region they are PR1a's exactly);
// - the budget rule (ADR 4.5): visible objects reserve their need first, in a deterministic screen-importance order with
//   ties broken by the stable instance key; the world keeps a fixed floor of each performance cap; an object that does
//   not fit the object share is refused whole, with a reason, never drawn partially;
// - the per-view instance draw records: one DrawnNodeGPU per (admitted row, chunk), with the offset T - camera formed in
//   double (the payload is asset-local, so the node centre c of R s c + T - camera is 0).
namespace gs_hlod_bundle {

// The world floor F (ADR 4.5, decision 3): the fraction of each performance cap of a view (the splat cap and the
// overlap-record cap) the world always keeps, whatever the objects ask for. Objects may reserve at most the rest.
// One half: the objects of a game frame can never take more than the world keeps, and the world's cut under
// half its caps is at most a little coarser (the selector refines in priority order, so a halved budget stops the
// same refinement sequence earlier; it never drops a region). An internal constant on purpose: a project setting
// would be a settings change (R3, follow-up F3 of the ADR).
constexpr uint32_t WORLD_FLOOR_NUMERATOR = 1;
constexpr uint32_t WORLD_FLOOR_DENOMINATOR = 2;

// The explicit refusal of an object that does not fit the object share (ADR 4.5).
constexpr const char *INSTANCE_REFUSAL_REASON = "hlod_instance_reserve_exceeds_object_share";

// One packed node asset. Chunk and record positions are region-local (0-based); the bundle places the region after the
// world: chunk table index node_count + first_chunk + c, atlas record world_records + first_record + i.
struct InstanceAsset {
	uint64_t asset_id = 0;
	uint32_t first_chunk = 0;
	uint32_t chunk_count = 0;
	uint64_t first_record = 0;
	uint64_t splat_count = 0;
	uint32_t max_chunk_splats = 0;
	// Overlap footprint of the asset at scale 1 (the HLOD bake's per-node rule: sum of 36 * s1 * s2 over the splats,
	// s1, s2 the two largest axes), so objects and world nodes are priced by one record-cost model.
	double footprint = 0.0;
	Vector3 bounds_center; // Asset-local.
	float bounds_radius = 0.0f;
};

struct InstanceRegion {
	LocalVector<InstanceAsset> assets; // Ascending asset_id; dense id = 1 + index (0 is the world).
	uint32_t chunk_count = 0;
	uint64_t records = 0;
	uint32_t max_chunk_splats = 0;
	// Row tallies (tally_rows): rows that name a packed node asset add that asset's chunks and splats; world rows count
	// separately (exactly one in a director-built bundle; the PR1a helpers take any number of world rows).
	uint64_t row_chunks = 0;
	uint64_t row_splats = 0;
	uint32_t world_rows = 1;

	int find(uint64_t p_asset_id) const {
		uint32_t lo = 0, hi = assets.size();
		while (lo < hi) {
			const uint32_t mid = lo + (hi - lo) / 2;
			if (assets[mid].asset_id < p_asset_id) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		return (lo < assets.size() && assets[lo].asset_id == p_asset_id) ? int(lo) : -1;
	}
};

// Dense id of an asset in a bundle with this region, or UINT32_MAX when the bundle does not pack it.
inline uint32_t dense_id_for_asset(uint64_t p_asset_id, const InstanceRegion &p_region) {
	if (p_asset_id == WORLD_ASSET_ID) {
		return WORLD_DENSE_ID;
	}
	const int index = p_region.find(p_asset_id);
	return index < 0 ? UINT32_MAX : uint32_t(index) + 1u;
}

// Fills the row tallies of `r_region` from `p_rows`. False when a row names an asset the region does not pack.
inline bool tally_rows(const Rows &p_rows, InstanceRegion &r_region) {
	r_region.row_chunks = 0;
	r_region.row_splats = 0;
	r_region.world_rows = 0;
	for (uint32_t i = 0; i < p_rows.asset_ids.size(); ++i) {
		if (p_rows.asset_ids[i] == WORLD_ASSET_ID) {
			++r_region.world_rows;
			continue;
		}
		const int index = r_region.find(p_rows.asset_ids[i]);
		if (index < 0) {
			return false;
		}
		r_region.row_chunks += r_region.assets[index].chunk_count;
		r_region.row_splats += r_region.assets[index].splat_count;
	}
	return true;
}

// Visible/draw-list capacity. Canonical (GPU-culled): a slot per (row, chunk) pair, world rows over every node. Relative
// (CPU-written): the world's node-indexed entries, then one entry per (instance row, chunk).
inline uint64_t visible_chunk_capacity(uint64_t p_node_count, bool p_relative, const InstanceRegion &p_region) {
	return p_relative ? p_node_count + p_region.row_chunks : p_node_count * uint64_t(p_region.world_rows) + p_region.row_chunks;
}

// nullptr when the rows can be packed with this region into a bundle of `p_node_count` world nodes; otherwise the reason.
// Row 0 must be the world (the director builds it first; every world draw record names row 0).
inline const char *rows_refusal(const Rows &p_rows, uint64_t p_node_count, const InstanceRegion &p_region) {
	const uint32_t n = p_rows.count();
	if (n == 0) { return "hlod_bundle_rows_empty"; }
	if (p_rows.grading.size() != n || p_rows.asset_ids.size() != n) { return "hlod_bundle_rows_mismatched"; }
	if (p_rows.asset_ids[WORLD_ROW] != WORLD_ASSET_ID) { return "hlod_world_row_unavailable"; }
	for (uint32_t i = 0; i < n; ++i) {
		if (dense_id_for_asset(p_rows.asset_ids[i], p_region) == UINT32_MAX) { return "hlod_bundle_row_asset_not_packed"; }
	}
	InstanceRegion tallied = p_region;
	if (!tally_rows(p_rows, tallied) || tallied.row_chunks != p_region.row_chunks || tallied.row_splats != p_region.row_splats ||
			tallied.world_rows != p_region.world_rows) {
		return "hlod_bundle_rows_region_mismatch";
	}
	// Every buffer of the bundle is created with a 32-bit size (hlod_create_buffer).
	if (uint64_t(n) * sizeof(InstanceDataGPU) > UINT32_MAX ||
			visible_chunk_capacity(p_node_count, false, p_region) * sizeof(VisibleChunkRefGPU) > UINT32_MAX ||
			visible_chunk_capacity(p_node_count, true, p_region) * sizeof(DrawnNodeGPU) > UINT32_MAX ||
			(p_node_count + p_region.chunk_count) * sizeof(ChunkQuantizationGPU) > UINT32_MAX) {
		return "hlod_bundle_rows_exceed_capacity";
	}
	return nullptr;
}

// The rows exactly as the bundle uploads them: each row names its asset's dense id (ids[0]); every other byte is the
// director's row. Only valid for rows that passed rows_refusal() with the same region.
inline void pack_instance_rows(const Rows &p_rows, const InstanceRegion &p_region, LocalVector<InstanceDataGPU> &r_packed) {
	r_packed = p_rows.instances;
	for (uint32_t i = 0; i < r_packed.size(); ++i) {
		r_packed[i].ids[0] = dense_id_for_asset(p_rows.asset_ids[i], p_region);
	}
}

// Bytes of one HLOD bundle with an instance region: the PR1a ledger plus the region's atlas records and chunk tables,
// one AssetMetaGPU per packed node asset and the instance draw/visible slots. For an empty region this is
// bundle_bytes(..., p_rows) of hlod_bundle_rows.h exactly (world_rows == p_rows).
inline uint64_t bundle_bytes(uint64_t p_node_count, uint64_t p_records, bool p_relative, bool p_quantized, uint32_t p_capacity,
		uint32_t p_rows, const InstanceRegion &p_region) {
	return (p_records + p_region.records) * (p_quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian)) +
			(p_node_count + p_region.chunk_count) * (sizeof(ChunkMetaGPU) + sizeof(AssetChunkIndexGPU) + (p_quantized ? sizeof(ChunkQuantizationGPU) : 0)) +
			visible_chunk_capacity(p_node_count, p_relative, p_region) * (p_relative ? sizeof(DrawnNodeGPU) : sizeof(VisibleChunkRefGPU)) +
			(1u + uint64_t(p_region.assets.size())) * sizeof(AssetMetaGPU) + uint64_t(p_rows) * (sizeof(InstanceDataGPU) + sizeof(InstanceGradingGPU)) +
			uint64_t(p_capacity) * (sizeof(SplatRefGPU) + (p_relative ? sizeof(uint32_t) : 0)) +
			6 * sizeof(uint32_t) + 2 * sizeof(GaussianSplatting::IndirectDispatchLayout);
}

// The world's share of a splat cap: ceil(cap * F). The objects' share is the rest.
inline uint64_t world_floor_splats(uint64_t p_cap) {
	return (p_cap * WORLD_FLOOR_NUMERATOR + WORLD_FLOOR_DENOMINATOR - 1u) / WORLD_FLOOR_DENOMINATOR;
}
inline uint64_t object_share_splats(uint64_t p_cap) {
	return p_cap - world_floor_splats(p_cap);
}
inline double world_floor_records(double p_cap) {
	return p_cap * double(WORLD_FLOOR_NUMERATOR) / double(WORLD_FLOOR_DENOMINATOR);
}
inline double object_share_records(double p_cap) {
	return MAX(0.0, p_cap - world_floor_records(p_cap));
}
// The objects' share of the overlap records: at most C_view - F (C_view = the configured record cap, or the storage when
// none is configured), and never more than the allocated storage holds beyond the world's need (capped at its floor),
// so the world keeps min(world need, storage / 2) and is not degraded at all when the storage holds both.
inline double object_share_records(double p_view_cap, double p_storage, double p_world_need) {
	const double view = MAX(p_view_cap, p_storage);
	return MAX(0.0, MIN(object_share_records(view), p_storage - MIN(p_world_need, world_floor_records(p_storage))));
}

// The overlap storage is shared: the route grows it to the world's record need plus the objects' estimate
// (TileRenderer::ensure_hlod_overlap_capacity). The world's need is tracked apart from the storage, because a storage
// already grown for the objects is not the world's need: read as such, the same object demand is added on top of it
// again every frame, and a stationary view regrows the storage (reallocating the overlap buffers and rebuilding the
// sorter) until the record cap or the ledger headroom stops it.
struct OverlapWorldNeed {
	uint64_t world_records = 0; // The world's need the route last sized the storage for.
	uint64_t storage_records = 0; // The storage capacity right after that sizing (0: never sized).
};
// The world's own record need this frame: the tracked need while the storage is still the one the route last sized.
// Any other storage (the first frame, a measured-overflow growth, a new device) is read whole as the world's: at most one
// object estimate too much, once, after which the tracked need holds. Never below `p_seed` (the world's seed rule).
inline uint64_t world_overlap_need(const OverlapWorldNeed &p_tracked, uint64_t p_storage, uint64_t p_seed) {
	const bool same_storage = p_tracked.storage_records != 0 && p_storage == p_tracked.storage_records;
	return MAX(same_storage ? p_tracked.world_records : p_storage, p_seed);
}
// The absolute storage requirement of a view: the world's need plus the objects' estimate, counted once (32-bit clamp).
// Unchanged content gives an unchanged requirement, so a storage that already holds it is never grown again.
inline uint32_t world_plus_objects_overlap_records(uint64_t p_world_need, double p_object_records) {
	const double objects = std::isfinite(p_object_records) && p_object_records > 0.0 ? std::ceil(MIN(p_object_records, double(UINT32_MAX))) : 0.0;
	return uint32_t(MIN<uint64_t>(MIN<uint64_t>(p_world_need, UINT32_MAX) + uint64_t(objects), UINT32_MAX));
}
// What the route grows the storage to once the requirement above exceeds it: the world's need plus 3/2 of the objects'
// estimate (the slack grow_hlod_overlap_storage gives a measured overflow), so objects that move and whose estimate
// rises a little each frame do not reallocate the storage on every rise. The route falls back to the exact requirement
// when the slack does not fit the headroom. Whether to grow is still decided by the exact requirement.
inline uint32_t world_plus_objects_overlap_growth(uint64_t p_world_need, double p_object_records) {
	return world_plus_objects_overlap_records(p_world_need, std::isfinite(p_object_records) ? p_object_records * 1.5 : 0.0);
}

// Two caps (PR1b): S = max(1, max_splats) bounds what the WORLD can draw (the world contract clamps it to the world's
// data count), C = the view's performance cap the objects reserve from (C >= S; renderer get_hlod_view_splat_cap).
// The world's need W = min(leaf_count, S) is PR1a's world cap.
inline uint64_t world_need_splats(uint32_t p_leaf_count, int p_max_splats) {
	return MIN(uint64_t(p_leaf_count), uint64_t(MAX(1, p_max_splats)));
}

// Splat-ref capacity of the relative bundle: the world's need W plus what the rows can be admitted to (at most the
// object share of C), never above max(C, W). For an empty region it is PR1a's min(leaf_count, S).
inline uint32_t relative_capacity(uint32_t p_leaf_count, int p_max_splats, uint64_t p_view_cap, const InstanceRegion &p_region) {
	const uint64_t world = world_need_splats(p_leaf_count, p_max_splats);
	const uint64_t view = MAX(MAX(p_view_cap, uint64_t(1)), world);
	const uint64_t objects = MIN(p_region.row_splats, object_share_splats(view));
	return uint32_t(MIN(MIN(world + objects, view), uint64_t(UINT32_MAX)));
}
// The canonical (GPU-culled whole-tree) bundle holds every leaf and every row's whole asset. UINT32_MAX when it would
// not fit a 32-bit count.
inline uint32_t canonical_capacity(uint32_t p_leaf_count, const InstanceRegion &p_region) {
	const uint64_t total = uint64_t(p_leaf_count) + p_region.row_splats;
	return total > UINT32_MAX ? UINT32_MAX : uint32_t(total);
}

// One visible object's demand in one view.
struct ObjectDemand {
	uint64_t instance_key = 0; // Stable instance id (the node's ObjectID): breaks importance ties.
	uint32_t row = 0; // Bundle row (>= 1).
	uint32_t asset_index = 0; // Into InstanceRegion::assets.
	uint64_t splats = 0;
	uint64_t chunks = 0;
	double records = 0.0; // Overlap-record estimate (object_view_demand).
	double importance = 0.0; // Projected radius in pixels; larger is more important.
};

struct ObjectShare {
	uint64_t splats = 0;
	uint64_t nodes = 0;
	double records = 0.0;
};

struct Admission {
	LocalVector<uint32_t> admitted; // Indices into the demand array, in ascending row order.
	LocalVector<uint32_t> refused; // Indices into the demand array, in admission order.
	uint64_t splats = 0;
	uint64_t nodes = 0;
	double records = 0.0;
};

// Admission order: screen importance (descending), then the stable instance key, then the row. A NaN importance sorts
// as 0, so the order is total and the same view always admits the same objects.
inline bool admits_before(const ObjectDemand &p_a, const ObjectDemand &p_b) {
	const double ia = std::isfinite(p_a.importance) ? p_a.importance : 0.0;
	const double ib = std::isfinite(p_b.importance) ? p_b.importance : 0.0;
	if (ia != ib) { return ia > ib; }
	if (p_a.instance_key != p_b.instance_key) { return p_a.instance_key < p_b.instance_key; }
	return p_a.row < p_b.row;
}

// ADR 4.5: objects are admitted in admits_before() order while the cumulative reserve fits the object share of every
// cap. An object that does not fit is refused whole and the next one is still tried: objects are never dropped as a
// group because one of them does not fit.
inline void admit_objects(const ObjectDemand *p_demands, uint32_t p_count, const ObjectShare &p_share, Admission &r_admission) {
	r_admission = Admission();
	LocalVector<uint32_t> order;
	order.resize(p_count);
	for (uint32_t i = 0; i < p_count; ++i) { order[i] = i; }
	std::sort(order.ptr(), order.ptr() + p_count, [&](uint32_t a, uint32_t b) { return admits_before(p_demands[a], p_demands[b]); });
	for (uint32_t k = 0; k < p_count; ++k) {
		const ObjectDemand &d = p_demands[order[k]];
		const bool records_ok = std::isfinite(d.records) && d.records >= 0.0 && r_admission.records + d.records <= p_share.records;
		if (d.splats <= p_share.splats - r_admission.splats && d.chunks <= p_share.nodes - r_admission.nodes && records_ok) {
			r_admission.splats += d.splats;
			r_admission.nodes += d.chunks;
			r_admission.records += d.records;
			r_admission.admitted.push_back(order[k]);
		} else {
			r_admission.refused.push_back(order[k]);
		}
	}
	std::sort(r_admission.admitted.ptr(), r_admission.admitted.ptr() + r_admission.admitted.size(),
			[&](uint32_t a, uint32_t b) { return p_demands[a].row < p_demands[b].row; });
}

// The world cut's caps after the objects reserved theirs (ADR 4.5): splats min(W, C - R), where W = min(leaf_count, S) is
// the world's need and C the view cap (a streamed atlas caps it further); records: the overlap storage minus the objects'
// estimate (the storage is grown to hold both, see TileRenderer::ensure_hlod_overlap_capacity). The admission never
// reserves more than the object share C - F, so the world keeps min(W, F) at least; when W + R <= C nothing is taken
// from the world at all. Without objects these are the PR1a caps exactly.
inline void world_view_caps(uint32_t p_leaf_count, int p_max_splats, uint64_t p_view_cap, bool p_streaming, uint64_t p_stream_records,
		double p_records, const Admission &p_admission, uint64_t &r_splat_budget, double &r_record_budget) {
	const uint64_t need = world_need_splats(p_leaf_count, p_max_splats);
	const uint64_t view = MAX(MAX(p_view_cap, uint64_t(1)), need);
	const uint64_t world = MIN(need, view - MIN(p_admission.splats, view));
	r_splat_budget = p_streaming ? MIN(world, p_stream_records) : world;
	r_record_budget = p_records - p_admission.records;
}

// One object's view demand: false when its bounding sphere (asset bounds under R s + T) is outside the frustum (the
// object is not visible in this view: it reserves nothing and is not refused). Otherwise its importance (projected
// radius in pixels) and its overlap-record estimate, with the selector's cost rule (lod/hlod_runtime_selection.cpp):
// splats + footprint * f^2 / (d^2 * tile_area), d the distance to the near surface floored at
// kCostInsideDistanceFraction of the radius.
inline bool object_view_demand(const InstanceAsset &p_asset, const Vector3 &p_world_center, float p_uniform_scale,
		const Plane *p_frustum, bool p_frustum_enabled, const Vector3 &p_camera, double p_focal_pixels, double p_tile_area,
		double &r_importance, double &r_records) {
	const double scale = std::fabs(double(p_uniform_scale));
	const double radius = double(p_asset.bounds_radius) * scale;
	if (p_frustum_enabled && p_frustum) {
		for (int i = 0; i < 6; ++i) {
			if (double(p_frustum[i].distance_to(p_world_center)) > radius) { return false; }
		}
	}
	const double dx = double(p_world_center.x) - double(p_camera.x);
	const double dy = double(p_world_center.y) - double(p_camera.y);
	const double dz = double(p_world_center.z) - double(p_camera.z);
	const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	r_importance = radius * p_focal_pixels / MAX(dist, 1.0e-6);
	const double d = MAX(MAX(dist - radius, 1.0e-6), gs_hlod_runtime::kCostInsideDistanceFraction * radius);
	const double tile_area = p_tile_area > 0.0 ? p_tile_area : 1.0;
	r_records = double(p_asset.splat_count) +
			p_asset.footprint * scale * scale * p_focal_pixels * p_focal_pixels / (MAX(d * d, 1.0e-12) * tile_area);
	return true;
}

// The draw records of one admitted row: one DrawnNodeGPU per chunk of its asset, all naming the row (instance_id) and the
// chunk's table index after the world's `p_world_nodes` nodes, all with the row's per-view offset.
inline void append_instance_draws(const InstanceRegion &p_region, uint32_t p_world_nodes, uint32_t p_row, uint32_t p_asset_index,
		const Vector3 &p_camera_offset, LocalVector<DrawnNodeGPU> &r_draws) {
	const InstanceAsset &asset = p_region.assets[p_asset_index];
	for (uint32_t c = 0; c < asset.chunk_count; ++c) {
		r_draws.push_back(gs_hlod_gpu::make_drawn_node(p_row, p_world_nodes + asset.first_chunk + c, p_camera_offset));
	}
}

// The per-view offset of an instance record: T - camera in double (c = 0), converted to float last.
inline bool instance_camera_offset(const Vector3 &p_translation, const gs_world_frame::PreciseCoord &p_origin,
		const gs_world_frame::PreciseCoord &p_camera, Vector3 &r_offset) {
	gs_world_frame::PrecisePlacement placement;
	if (!gs_world_frame::PrecisePlacement::from_frame_transform(Transform3D(Basis(), p_translation), p_origin, placement)) {
		return false;
	}
	return gs_world_frame::camera_relative_offset(placement, gs_world_frame::PreciseCoord(), p_camera, r_offset);
}

} // namespace gs_hlod_bundle

#endif // GS_HLOD_WORLD_INSTANCES_H
