#pragma once

#include "core/string/ustring.h"

// The automatic HLOD route (docs/architecture/adr-one-route-many-producers.md §4.7, maintainer decision 5,
// 2026-11-04). A GaussianSplatWorld3D whose world can carry an HLOD snapshot (an HLOD tree over a resident payload, or
// a V4 paged package) takes the HLOD route by default, together with the node instances (rows 1..N of the same bundle), with no
// project setting changed. Pure, so the routing matrix is a CPU doctest; GaussianSplatWorld3D::_register_shared_renderer()
// reads the inputs and applies the plan.
//
// - rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled (default true) is the explicit opt-out. false restores
//   the routing from before the automatic route exactly: no snapshot is captured, route_policy decides the backend and
//   a V4 paged world loads its payload into memory and takes the resident route (hlod_paged_requires_runtime_cut).
// - rendering/gaussian_splatting/streaming/route_policy is not an opt-out. Its default is streaming, and a value set
//   equal to a default is indistinguishable from an unset one (docs/architecture/gaussian-project-settings-contract.md),
//   so "streaming" cannot mean "no HLOD". It keeps deciding the backend of every world that does not take the HLOD route.
// - Node instances do not change the decision: since PR1b they are rows of the world's HLOD bundle.
namespace gs_hlod_auto_route {

struct WorldInputs {
	bool cut_enabled = true; // lod/hlod_runtime_cut_enabled
	bool has_hlod_tree = false; // GaussianSplatWorld::has_hlod_tree()
	// GaussianSplatWorld::has_resident_gaussian_data(). A tree over a file-backed (streamable) payload - what a generic
	// ResourceSaver::save() and the world importer write for a streamable source - cannot be snapshotted (the runtime
	// cut needs the resident payload), so such a world is not an HLOD-route world: it keeps its route and reports nothing.
	bool resident_payload = false;
	bool paged = false; // GaussianSplatWorld::has_hlod_paged_source(), after the paged fallback load (if any) ran
	bool identity_transform = true; // the world node's global transform is the identity
	bool route_policy_streaming = true; // route_policy == streaming (the project default)
	// A setup of this world that the HLOD route never serves, known at submission (nullptr, or one of the keys of
	// GaussianSplatWorld3D::_hlod_auto_route_refusal(): lod_disabled, deformation_or_instance_domain, painterly_enabled,
	// shadow_instance_filter).
	const char *domain_refusal = nullptr;
};

struct WorldPlan {
	bool capture_snapshot = false;
	// The submission's hlod_reason when no snapshot is captured. Empty for a world the HLOD route does not concern
	// (no tree, or the opt-out set): such a world reports exactly what it reported before the automatic route.
	String reason;
};

// Prefix of the reason a world with an HLOD tree reports when the automatic route keeps it on the chunk-streaming route.
static constexpr const char *AUTO_ROUTE_UNAVAILABLE_PREFIX = "hlod_auto_route_unavailable:";

inline WorldPlan plan_world(const WorldInputs &p_in) {
	WorldPlan plan;
	if (!p_in.cut_enabled || (!(p_in.has_hlod_tree && p_in.resident_payload) && !p_in.paged)) {
		return plan;
	}
	if (!p_in.identity_transform) {
		plan.reason = "hlod_world_transform_unsupported";
		return plan;
	}
	// Under the default streaming policy a world whose setup the HLOD route never serves keeps the chunk route it has
	// without a snapshot: the HLOD route would fall back to a canonical resident copy, which the atlas budget thins
	// where the chunk route streams. Under route_policy = resident the world is on the resident backend either way, and
	// the HLOD publisher serves these setups with its canonical leaves and resumes the cut when they end (unchanged).
	// A paged world here could not load its payload (its own reason is already set); it has no other route.
	if (p_in.route_policy_streaming && p_in.domain_refusal != nullptr && !p_in.paged) {
		plan.reason = String(AUTO_ROUTE_UNAVAILABLE_PREFIX) + p_in.domain_refusal;
		return plan;
	}
	plan.capture_snapshot = true;
	return plan;
}

// The submission's residency hint: a captured snapshot takes the HLOD route, which runs on the resident backend;
// every other world keeps route_policy's hint.
inline bool residency_hint_resident(bool p_route_policy_streaming, bool p_snapshot_captured) {
	return p_snapshot_captured || !p_route_policy_streaming;
}

} // namespace gs_hlod_auto_route
