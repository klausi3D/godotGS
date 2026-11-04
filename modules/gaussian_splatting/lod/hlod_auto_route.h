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
	// The node was demoted (see "Sticky demotion" below): it keeps the route it had without the automatic route until
	// it is applied again. The cause is the caller's; the plan only needs to know.
	bool demoted = false;
};

struct WorldPlan {
	bool capture_snapshot = false;
	// The submission's hlod_reason when no snapshot is captured. Empty for a world the HLOD route does not concern
	// (no tree, or the opt-out set): such a world reports exactly what it reported before the automatic route.
	String reason;
};

// Prefix of the reason a world with an HLOD tree reports when the automatic route keeps it on the chunk-streaming route.
static constexpr const char *AUTO_ROUTE_UNAVAILABLE_PREFIX = "hlod_auto_route_unavailable:";
// Prefix of the reason of a demoted world (the cause follows).
static constexpr const char *AUTO_ROUTE_DEMOTED_PREFIX = "hlod_auto_route_demoted:";

inline WorldPlan plan_world(const WorldInputs &p_in) {
	WorldPlan plan;
	if (!p_in.cut_enabled || (!(p_in.has_hlod_tree && p_in.resident_payload) && !p_in.paged)) {
		return plan;
	}
	if (p_in.demoted) {
		return plan; // The caller states hlod_auto_route_demoted:<cause>; a paged world loaded its payload for it.
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

// Sticky demotion. Some refusals only appear once the world is on the route: a view the route does not serve
// (orthographic, multi-view/XR, painterly), a domain that starts after the submission (painterly, the shadow instance
// filter, effector nodes, wind), or quantization over a tree with mixed DC encodings. Where the world has a canonical copy
// that serves the frame (a whole-tree resident bundle) the publisher's own fallback draws it. Where it has none (a
// streamed or paged world), or where that copy is a resident one the atlas budget thins (a world too large for its whole
// tree), the world would draw less than without the automatic route. Such a refusal demotes the world: the renderer asks
// once, the node resubmits it without a snapshot (a paged world loads its payload into memory), and it stays on that
// route until it is applied again. Re-promotion is a follow-up.
enum class RefusalStage {
	FRAME, // a per-view refusal in prepare_hlod_frame()
	PUBLISH, // a refusal while the bundle is published (try_publish_hlod_resident_contract())
};

struct RefusalInputs {
	RefusalStage stage = RefusalStage::FRAME;
	const char *reason = nullptr;
	bool main_view = true; // FRAME: the pass is the camera's main view (shadow and manual passes never demote)
	// The world has no canonical copy that serves the frame: it streams (FRAME), or it is paged or too large for its
	// whole tree (PUBLISH).
	bool no_canonical_copy = false;
	bool committed_edit = false; // an edited world is served by the streamer only; it is never demoted to unedited data
};

// The cause to demote for, or nullptr.
inline const char *demotion_cause(const RefusalInputs &p_in) {
	if (p_in.reason == nullptr || p_in.committed_edit) {
		return nullptr;
	}
	const String reason = p_in.reason;
	if (reason == "hlod_quantized_mixed_dc_node") {
		// The quantized route refuses the tree outright: the canonical copy is the resident one, which a paged world does
		// not have and a large world gets thinned. master quantizes it on the chunk route or loads it into memory.
		return p_in.stage == RefusalStage::PUBLISH ? "quantized_mixed_dc_node" : nullptr;
	}
	if (!p_in.no_canonical_copy) {
		return nullptr;
	}
	if (p_in.stage == RefusalStage::FRAME) {
		if (!p_in.main_view) {
			return nullptr;
		}
		if (reason == "unsupported_view_or_material") { return "unsupported_view_or_material"; }
		if (reason == "deformation_or_instance_domain" || reason == "hlod_paged_deformation_or_instance_domain") { return "deformation_or_instance_domain"; }
		if (reason == "lod_disabled_canonical_leaves" || reason == "hlod_paged_lod_disabled") { return "lod_disabled"; }
		return nullptr;
	}
	// PUBLISH: GaussianSplatRenderer::hlod_streaming_unsupported_reason() keys.
	if (reason == "lod_disabled_canonical_leaves") { return "lod_disabled"; }
	if (reason == "shadow_instance_filter") { return "shadow_instance_filter"; }
	if (reason == "unsupported_view_or_material") { return "painterly_enabled"; }
	if (reason == "deformation_or_instance_domain") { return "deformation_or_instance_domain"; }
	if (reason == "unsupported_instance_transform_domain") { return "unsupported_instance_transform_domain"; }
	return nullptr;
}

// One demotion request per submitted snapshot: the first refusal asks, later frames do not ask again. A new snapshot
// (the world applied again) re-arms it.
struct DemotionLatch {
	bool requested = false;
	String cause;
	bool request(const char *p_cause) {
		if (requested || p_cause == nullptr) {
			return false;
		}
		requested = true;
		cause = p_cause;
		return true;
	}
	void rearm() {
		requested = false;
		cause = String();
	}
};

} // namespace gs_hlod_auto_route
