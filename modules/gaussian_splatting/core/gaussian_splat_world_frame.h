#ifndef GAUSSIAN_SPLAT_WORLD_FRAME_H
#define GAUSSIAN_SPLAT_WORLD_FRAME_H

// ADR docs/architecture/adr-hlod-streaming.md sections 6.2, 6.9 and 14.3 (package C, slice S2b).
//
// The precise CPU world frame: a flat Cartesian frame whose positions are an integer lattice
// address plus a double offset inside the lattice cell. Nothing here is a bound API.
//
//   - PreciseCoord: lattice address (int64 per axis, edge LATTICE_EDGE metres) plus a double
//     offset in [0, LATTICE_EDGE). Differences of two coordinates are formed on the integer
//     addresses first, so they are exact at any distance from the frame origin.
//   - PrecisePlacement: rotation*uniform scale (float basis) plus a PreciseCoord translation.
//   - WorldOriginContext: the origin of the GS render frame. Every origin change bumps a
//     revision; frame-dependent caches and snapshots key on it. It never touches scene nodes.
//
// Payloads stay node-local floats; a view-dependent offset is only ever computed per draw
// (camera_relative_offset) and is never stored with shared payloads.

#include "core/math/basis.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/typedefs.h"

namespace gs_world_frame {

// 2^10 m. A power of two, so cell*edge and all lattice arithmetic below are exact in double.
constexpr int LATTICE_EXP = 10;
constexpr double LATTICE_EDGE = 1024.0;
// Largest accepted |cell| so differences of two coordinates cannot overflow int64.
constexpr int64_t MAX_CELL = int64_t(1) << 60;

struct PreciseCoord {
	int64_t cell[3] = { 0, 0, 0 };
	double local[3] = { 0.0, 0.0, 0.0 }; // In [0, LATTICE_EDGE).

	// Splits finite doubles exactly (no rounding of the input). False for non-finite or out-of-range input.
	static bool from_doubles(const double p_value[3], PreciseCoord &r_out);
	// Centre of lattice cell (ix, iy, iz) of edge 2^p_e (an HLOD tree cell, ADR 6.1) on a double origin.
	// The cell address is integer arithmetic; the double origin is split exactly, so no rounded absolute
	// coordinate is ever involved.
	static bool from_tree_cell_center(const double p_origin[3], int32_t p_e, int64_t p_ix, int64_t p_iy, int64_t p_iz,
			PreciseCoord &r_out);

	// this - p_base, in double, via the integer addresses. Exact up to double rounding of the (small) result.
	bool offset_from(const PreciseCoord &p_base, double r_offset[3]) const;
	// this + p_delta, renormalised. False when the result leaves the supported range.
	bool translated(const double p_delta[3], PreciseCoord &r_out) const;
	// Lossy: the nearest double of the absolute position (diagnostics and tests only).
	void to_doubles(double r_out[3]) const;

	bool operator==(const PreciseCoord &p_other) const;
	bool operator!=(const PreciseCoord &p_other) const { return !(*this == p_other); }
};

// World placement of an instance: world = basis * asset_position + translation. The basis carries rotation and
// uniform scale (anything else is refused by the consumers).
struct PrecisePlacement {
	Basis basis;
	PreciseCoord translation;

	// Lifts a float transform that is already expressed in a frame whose origin is `p_frame_origin`.
	// Only as precise as the float input: use for scene nodes, never for imported world data.
	static bool from_frame_transform(const Transform3D &p_transform, const PreciseCoord &p_frame_origin, PrecisePlacement &r_out);
	// The transform of this placement in the frame whose origin is `p_frame_origin` (double subtraction, then float).
	bool to_frame_transform(const PreciseCoord &p_frame_origin, Transform3D &r_out) const;
};

// Per-draw camera-relative offset `R s c + T - camera` (ADR 6.2 "Per drawn entry (S2b)"), computed in double
// from the precise representation and converted to float last. `p_node_center` is the node's cell centre in
// asset space. The result is view dependent: never store it with a shared payload.
bool camera_relative_offset(const PrecisePlacement &p_placement, const PreciseCoord &p_node_center,
		const PreciseCoord &p_camera, Vector3 &r_offset);

// Receives origin changes. This is the explicit integration point for external gameplay/physics origin handling:
// the module never moves scene nodes itself.
class OriginShiftHook {
public:
	virtual ~OriginShiftHook() {}
	virtual void on_world_origin_changed(const PreciseCoord &p_old, const PreciseCoord &p_new, uint64_t p_revision) = 0;
};

// Origin of the GS render frame. Owned by the scene director (one per module). Not thread-safe: main thread only.
class WorldOriginContext {
public:
	const PreciseCoord &get_origin() const { return origin; }
	// Starts at 0 and increments by one on every change of the origin; unchanged sets do not bump it.
	uint64_t get_revision() const { return revision; }

	// False (and no change) when the origin is not representable.
	bool set_origin(const PreciseCoord &p_origin);
	bool shift_origin_by(const double p_delta[3]);

	// Hooks are not owned; the caller removes them before destruction.
	void add_hook(OriginShiftHook *p_hook);
	void remove_hook(OriginShiftHook *p_hook);

private:
	PreciseCoord origin;
	uint64_t revision = 0;
	LocalVector<OriginShiftHook *> hooks;
};

} // namespace gs_world_frame

#endif
