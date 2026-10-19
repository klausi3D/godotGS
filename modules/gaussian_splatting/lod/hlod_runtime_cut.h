#pragma once

#ifdef TESTS_ENABLED

#include "../core/gaussian_splat_world.h"

// Internal CPU falsifier, not a renderer/publication API. Call on the main thread
// with a quiescent, single resident source world; no concurrent source edits.
// The transient result contains render payload/chunks/bounds, not unbounded
// authoring metadata, instances, persistence or animation state.
namespace gs_hlod::runtime_cut {

constexpr uint32_t MAX_NODES = 4096;
constexpr uint32_t MAX_INPUT_SPLATS = 65536; // Leaves plus all interior payloads.
constexpr uint64_t MAX_ALLOCATION_BYTES = 128u * 1024u * 1024u;

enum class Status {
	COMPLETE,
	EMPTY_INPUT,
	INVALID_INPUT,
	UNSUPPORTED,
	STALE,
	UNAVAILABLE,
	BUDGET_INFEASIBLE,
	ALLOCATION_FAILED
};

struct View {
	Vector3 position;
	Vector3 forward = Vector3(0, 0, -1); // Normalized internally; asset coordinates.
	double focal_pixels = 1000;
	double near_plane = 0.1;
};

struct Request {
	uint64_t source_revision = 0; // Required exact leaf revision, not a wildcard.
	Vector<View> views;           // One or two static views; no frustum removal.
	double threshold_pixels = 1;
	uint64_t splat_budget = MAX_INPUT_SPLATS;
	uint64_t allocation_budget_bytes = MAX_ALLOCATION_BYTES;
	// Empty means all physically resident payloads. Otherwise one 0/1 per node;
	// this can withhold resident data, never make nonresident data available.
	Vector<uint8_t> payload_available;
	bool force_leaves = false; // Canonical-representation oracle, still budgeted.
	bool deformation = false;  // Unsupported, always rejected.
};

struct Result {
	Status status = Status::INVALID_INPUT;
	String reason;
	bool coverage_complete = false;
	bool budget_satisfied = false;
	bool heuristic_available = false;
	bool heuristic_satisfied = false;
	bool canonical_leaves = false;
	bool position_rounding_occurred = false;
	uint64_t source_revision = 0;
	uint64_t selected_splats = 0;
	uint64_t admitted_allocation_bytes = 0; // Conservative admission estimate, not measured RSS.
	uint64_t cpu_usec = 0;
	Vector<uint32_t> selected_nodes; // Deterministic child-order traversal.
	Ref<GaussianSplatWorld> world;   // Complete independent transient result, or null on failure.
};

// Pixel heuristic = geometric_error * focal_pixels / nearest AABB depth, worst
// across views. This is NOT a proved radiance/image-error bound. Near/behind
// bounds refine; if only a budgeted parent or leaves remain, the heuristic is
// explicitly unavailable/unsatisfied. Failure returns no partial cut/world.
Result build_cut(const Ref<GaussianSplatWorld> &p_source, const Request &p_request);

} // namespace gs_hlod::runtime_cut
#endif
