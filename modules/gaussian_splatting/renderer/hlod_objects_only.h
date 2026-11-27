#ifndef GS_HLOD_OBJECTS_ONLY_H
#define GS_HLOD_OBJECTS_ONLY_H

#include "core/string/ustring.h"

#include <cstdint>

// The objects-only HLOD bundle: the P0-absent mode of the one route (docs/architecture/adr-one-route-many-producers.md
// section 12.7, PR-1). A frame whose renderer has node instances (rows 1..N) but no world cut (P0) - no world at all, or
// a world without an HLOD snapshot - is drawn on the HLOD route with an empty row 0: no snapshot, no world node, no
// ViewSet, and no world floor. Pure, so the routing matrix is a CPU doctest; the publisher reads the inputs and applies
// the plan (GaussianSplatRenderer::try_publish_hlod_resident_contract()).
//
// - Owner decision OD-2 (2026-11-27): the opt-out rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled = false does
//   not keep an objects-only bundle off the route. It only names the cause of a dropped world.
// - A world submission without a snapshot next to node instances is dropped explicitly: the canonical routes never drew
//   it next to a node either (the world row is emitted only without node instances, director build_instance_buffer_for_renderer;
//   measured on a GPU before PR-1). The plan names why (`world_reason`); the renderer reports it as the render stat
//   hlod_world_producer_reason and counts the frames in hlod_world_producer_dropped_frames.
// - Without node instances nothing changes: a world without a snapshot keeps its route and draws.
namespace gs_hlod_objects_only {

static constexpr const char *WORLD_WITHOUT_SNAPSHOT_PREFIX = "hlod_world_without_snapshot:";
// Causes when the world submission names none (the automatic route reports no reason for a world it does not concern).
static constexpr const char *CAUSE_RUNTIME_CUT_DISABLED = "runtime_cut_disabled";
static constexpr const char *CAUSE_NO_RUNTIME_TREE = "no_runtime_tree";
static constexpr const char *CAUSE_SUBMISSION_NOT_OWNED = "world_submission_not_owned";

struct Inputs {
	// The world path is available: a current snapshot, the cut enabled and the world submission owned by this renderer's
	// owner key. The world path decides the frame then (it may still refuse; that is unchanged).
	bool world_route = false;
	// Visible node instances with asset data for this renderer (rows 1..N of the bundle).
	uint32_t node_rows = 0;
	// A renderable world submission exists for this renderer.
	bool world_submission = false;
	// The renderer holds a snapshot (only used to name the cause).
	bool snapshot = false;
	bool cut_enabled = true;
	// The submission's hlod_reason (the automatic route's reason; empty when it names none).
	String submission_reason;
};

struct Plan {
	bool objects_only = false;
	// Empty when no world was submitted; otherwise WORLD_WITHOUT_SNAPSHOT_PREFIX + cause.
	String world_reason;
};

inline Plan plan(const Inputs &p_in) {
	Plan out;
	if (p_in.world_route || p_in.node_rows == 0) {
		return out;
	}
	out.objects_only = true;
	if (!p_in.world_submission) {
		return out;
	}
	String cause = p_in.submission_reason;
	if (cause.is_empty()) {
		if (!p_in.cut_enabled) {
			cause = CAUSE_RUNTIME_CUT_DISABLED;
		} else if (p_in.snapshot) {
			cause = CAUSE_SUBMISSION_NOT_OWNED;
		} else {
			cause = CAUSE_NO_RUNTIME_TREE;
		}
	}
	out.world_reason = String(WORLD_WITHOUT_SNAPSHOT_PREFIX) + cause;
	return out;
}

} // namespace gs_hlod_objects_only

#endif // GS_HLOD_OBJECTS_ONLY_H
