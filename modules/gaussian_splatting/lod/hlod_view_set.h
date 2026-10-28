#ifndef GS_HLOD_VIEW_SET_H
#define GS_HLOD_VIEW_SET_H

// ADR docs/architecture/adr-hlod-streaming.md sections 14.1, 14.2 and 6.9 (package B2).
//
// CPU-only contracts that sit on top of the existing S2a selector (`CutHistory`); there is no
// second selector. Nothing here is a bound API or a registered class. The renderer owns its
// per-view cut state through this ViewSet (RenderFrameContextManager, package B3).
//
//   - NodeKey / NodeVersion: stable node identity and content identity, independent of atlas
//     address and of node-array order.
//   - SelectionSnapshot: an immutable input (hierarchy/content revision, transforms, residency,
//     budgets, per-view parameters). Selection only reads it.
//   - Frame (package C, ADR 14.3): the snapshot carries the render-frame origin (precise) and its revision.
//     Instance placements and node centres are precise; selection works in the origin-relative float frame
//     derived from them. A different origin revision invalidates every view's cut history and published draw
//     set before anything is selected, so old and new offsets never mix.
//   - ViewSet: each admitted view owns its cut histories and its published draw records.
//     One physical byte budget covers all views and instances: shared payload is charged once per
//     asset, per-view sort/record/draw allocations are charged once per admitted view.
//     Admitting a view either reserves everything it needs or returns an explicit refusal; no
//     other view's caps, history or published draw records are touched by either outcome.
//
// Threading: not thread-safe; call from the thread that owns the selection state.

#include "../core/gaussian_splat_world_frame.h"
#include "hlod_runtime_selection.h"

namespace gs_hlod_runtime {

struct NodeKey {
	// Stable world/asset identity chosen by the caller (for example a resource UID hash). It must
	// not be an object instance id, which is not stable across sessions.
	uint64_t asset_key = 0;
	// World-lattice cell of the node (ADR 6.1): independent of the node's array position.
	int32_t cell_e = 0;
	int64_t cell_x = 0;
	int64_t cell_y = 0;
	int64_t cell_z = 0;
	// Topology-shape flags that distinguish nodes sharing a cell (split-by-index, grouped leaf,
	// origin-centred root). The representation kind is content, not identity.
	uint32_t shape = 0;
	// Nodes of an index-split group share one cell: ordinal of each ancestor-or-self within its
	// parent's children, packed 3 bits per level, with the level count in `split_depth`.
	uint32_t split_depth = 0;
	uint64_t split_path = 0;

	bool operator==(const NodeKey &p_other) const;
	bool operator<(const NodeKey &p_other) const; // Total order used for deterministic publication.
};

struct NodeVersion {
	uint64_t content_revision = 0;
	uint32_t bake_rule_revision = 0;
	bool operator==(const NodeVersion &p_other) const {
		return content_revision == p_other.content_revision && bake_rule_revision == p_other.bake_rule_revision;
	}
};

// Computes the stable key of `p_node`. Returns false for an out-of-range node or a corrupt parent
// chain; the key is then unspecified. Cost is O(split group depth), not O(nodes).
bool make_node_key(const GaussianSplatHlodRuntimeData &p_data, uint32_t p_node, uint64_t p_asset_key, NodeKey &r_key);

// Per-instance (or per-view) caps, in the units `CutRequest` uses.
struct Caps {
	uint64_t splat_budget = 0;
	uint32_t node_budget = 0;
	double record_budget = 0.0;
};

struct SnapshotAsset {
	uint64_t asset_key = 0;
	Ref<GaussianSplatHlodRuntimeData> data;
	uint32_t bake_rule_revision = 0;
	// One byte per node, 1 = resident and completely uploaded. Empty means everything is resident.
	Vector<uint8_t> resident;
	// Allocated (not merely selected) payload bytes charged for this asset, shared by all of its
	// instances and views. Counted once per distinct `asset_key`.
	uint64_t payload_bytes = 0;
};

struct SnapshotInstance {
	uint64_t instance_key = 0; // Stable placement identity; breaks ties and orders publication.
	uint32_t asset_index = 0;
	// World placement (precise): rigid plus uniform scale; anything else is refused. The selection frame
	// transform is derived from it and the snapshot origin in double.
	gs_world_frame::PrecisePlacement placement;
	// This instance's share of every view's caps. Admission requires the sum over instances to fit
	// the view's caps, so a view never exceeds what was reserved for it. A shared cut across
	// instances needs the top-level tree (ADR 6.9, slice S2b) and is deferred.
	Caps caps;
};

struct ViewSpec {
	uint64_t view_key = 0;
	Vector3 camera; // Frame space: world minus the snapshot origin. Float: used for selection.
	// Camera contract: when `camera_precise_valid` the per-draw offset uses `camera_precise` (absolute, source
	// precision). Otherwise it is origin + `camera`, which is only as precise as the float `camera`: a float
	// camera 100 km from the origin is already rounded to millimetres. The origin must therefore be kept near
	// the camera; shifting the origin is what keeps a float-only camera precise (ADR 14.3).
	gs_world_frame::PreciseCoord camera_precise;
	bool camera_precise_valid = false;
	Plane frustum[6]; // Frame space.
	bool frustum_enabled = true;
	double focal_pixels = 0.0;
	double tau_pixels = 1.0;
	double tile_area = 256.0;
	Caps caps; // Whole-view caps; also what admission reserves.
};

struct ResidencyBudget {
	uint64_t physical_bytes = 0; // One budget across views and instances.
	uint64_t sort_bytes_per_splat = 0;
	uint64_t record_bytes_per_record = 0;
	uint64_t draw_bytes_per_node = 0;
	uint32_t max_views = 0;
};

// Immutable once built: selection and admission take it by const reference and never keep it.
struct SelectionSnapshot {
	uint64_t hierarchy_revision = 0; // Caller-defined counter, echoed into results.
	// Origin of the render frame and its revision (gs_world_frame::WorldOriginContext). Any change of the
	// revision invalidates frame-dependent state in ViewSet::select().
	gs_world_frame::PreciseCoord origin;
	uint64_t origin_revision = 0;
	Vector<SnapshotAsset> assets;
	Vector<SnapshotInstance> instances;
	Vector<ViewSpec> views; // Per-frame parameters of the active views.
	ResidencyBudget budget;
};

struct DrawRecord {
	uint64_t instance_key = 0;
	uint32_t node = 0; // Index into the asset's node table; valid only for this snapshot.
	NodeKey key;
	NodeVersion version;
	// Per-draw camera-relative offset (R s c + T - camera), computed in double from the precise placement and
	// node centre and converted to float last. View dependent, so it lives here and never in shared payloads.
	// Valid only for the origin revision reported by drawn_origin_revision().
	Vector3 camera_offset;
};

// Desired(view), demand only (ADR 14.1): a node the quality rule wants resident that is not. It carries no
// atlas address and never makes anything drawable; the residency layer decides what is requested and when a
// node counts as resident-ready. Ordered by the selector's refinement priority (highest first), children of
// one parent consecutive.
struct DesiredRecord {
	uint64_t instance_key = 0;
	uint32_t node = 0; // Index into the asset's node table; valid only for this snapshot.
	// false: a node the view wants and does not have (request it). true: a node that is already resident and is
	// part of the target the view is building (the resident part of an incomplete bootstrap frontier): keep it,
	// do not request it.
	bool keep = false;
};

class ViewSet {
public:
	enum AdmitStatus {
		ADMITTED,
		REFUSED_INVALID, // Malformed snapshot or spec.
		REFUSED_DUPLICATE, // The view key is already admitted.
		REFUSED_VIEW_LIMIT,
		REFUSED_CAPS, // Instance caps do not fit the view's caps (or their sum overflows).
		REFUSED_BUDGET, // Shared payload plus all admitted views plus this one exceed the budget (or overflow).
		REFUSED_RESOURCES, // A cut workspace or draw storage could not be allocated.
	};
	struct AdmitResult {
		AdmitStatus status = REFUSED_INVALID;
		uint64_t view_bytes = 0; // This view's full charge (see view_cost).
		uint64_t charged_bytes = 0; // Shared payload plus already-admitted views.
		uint64_t budget_bytes = 0;
	};
	// Semantic readiness (ADR 14.2); not a bound enum.
	enum Readiness {
		LOADING, // Residency for the complete frontier is still missing.
		READY, // A complete coarse representation (every selected node resident) is published.
		NOT_ADMITTED, // The configured budgets/caps/reservation cannot hold a complete frontier.
		FAILED, // Invalid input, arithmetic or an allocation failure.
	};
	struct ViewResult {
		uint64_t view_key = 0;
		Readiness readiness = FAILED;
		CutHistory::SelectStatus worst_status = CutHistory::COMPLETE; // The first non-success instance status.
		uint32_t drawn_count = 0;
		uint64_t selected_splats = 0;
		uint64_t visited_nodes = 0; // Summed over the view's instances.
		double estimated_records = 0.0;
		bool budget_bound = false;
		uint64_t draw_generation = 0; // Bumped only when a new complete draw set is published.
		uint64_t origin_revision = 0; // Origin revision this selection was computed against.
		bool origin_changed = false; // The revision differs from the previous selection: histories were invalidated.
		// The snapshot asks for more than the view reserved at admission (caps, instances, history or draw
		// storage), or the shared budget no longer holds; nothing was selected or published for this view.
		// The caller must re-admit the view against the new snapshot.
		bool reservation_exceeded = false;
		// Number of Desired(view) records produced by this selection (see desired()), including for a LOADING
		// view, and whether lowest-priority demand was dropped because the view's reserved storage was full.
		uint32_t desired_count = 0;
		bool desired_truncated = false;
	};
	enum SnapshotStatus {
		SNAPSHOT_OK,
		SNAPSHOT_INVALID,
		SNAPSHOT_OVER_BUDGET, // Shared payload plus reserved views exceed the budget; no view was selected.
	};
	struct Ledger {
		uint64_t shared_payload_bytes = 0;
		uint64_t view_bytes = 0;
		uint64_t total() const { return shared_payload_bytes + view_bytes; }
	};

	AdmitResult admit_view(const SelectionSnapshot &p_snapshot, const ViewSpec &p_spec);
	bool remove_view(uint64_t p_view_key);
	uint32_t view_count() const { return views.size(); }

	// Runs one selection per admitted view that appears in the snapshot, in ascending view key, and stores
	// the per-view results (see results()). In steady state (same instance and view sets, same node counts)
	// it performs no heap allocation: every buffer was sized by admit_view(), and the only growth is the
	// scratch used to order/validate a larger snapshot.
	// A view that cannot be complete reports its state and publishes nothing new; drawn() then returns no
	// records. Other views are unaffected.
	SnapshotStatus select(const SelectionSnapshot &p_snapshot);
	// Results of the last select(), ascending view key. Valid until the next select/admit/remove.
	const ViewResult *results(uint32_t &r_count) const {
		r_count = result_count;
		return result_store.ptr();
	}

	// Published draw records of an admitted view, ordered by (instance key, NodeKey) and therefore
	// independent of node-array order. Count is 0 (and the pointer null) unless the view's last selection
	// was READY, so a stale set can never be mistaken for current coverage.
	const DrawRecord *drawn(uint64_t p_view_key, uint32_t &r_count) const;
	// Desired(view) of the last select(): non-resident nodes the quality rule wants. Unlike drawn() it is also
	// available while the view is LOADING (that is when it matters). Count 0 and null for an unknown view or
	// when the view was not selected. Valid until the next select/admit/remove.
	const DesiredRecord *desired(uint64_t p_view_key, uint32_t &r_count) const;
	// Origin revision every record of the view's published draw set was computed against (0 when none).
	uint64_t drawn_origin_revision(uint64_t p_view_key) const;

	// Shared payload (once per distinct asset key) plus the reservations of the admitted views.
	// Returns false on overflow or a malformed snapshot.
	bool ledger(const SelectionSnapshot &p_snapshot, Ledger &r_ledger) const;
	// Full charge of one view: GPU-side sort/record/draw bytes from its caps, plus the actually allocated
	// CPU side (cut workspaces and tie ranks, and the published plus staging draw records).
	static bool view_cost(const ResidencyBudget &p_budget, const Caps &p_caps, uint64_t p_history_bytes,
			uint64_t p_draw_capacity, uint64_t &r_bytes);
	// Bytes one instance slot allocates for an asset with `p_nodes` nodes.
	static uint64_t slot_bytes(uint32_t p_nodes);
	// Capacity (bytes) of everything this set owns: slots, draw storage, results and scratch.
	uint64_t allocated_bytes() const;
	// Number of node-key rank tables computed so far (O(n log n) each); admissions of unchanged content reuse one.
	uint64_t tie_rank_builds() const { return rank_builds; }

private:
	struct InstanceSlot {
		uint64_t instance_key = 0;
		uint32_t node_count = 0;
		uint64_t content_revision = 0;
		uint64_t policy = 0;
		CutHistory history;
		Vector<uint32_t> tie_rank; // Rank of each node's NodeKey: array-order independent tie-break.
	};
	struct View {
		uint64_t view_key = 0;
		Caps reserved;
		uint64_t reserved_bytes = 0;
		uint64_t reserved_history_bytes = 0;
		uint32_t reserved_draw_capacity = 0;
		Vector<InstanceSlot> slots;
		Vector<DrawRecord> drawn;
		Vector<DrawRecord> staging;
		Vector<DesiredRecord> desired;
		uint32_t desired_count = 0;
		uint32_t drawn_count = 0;
		uint64_t draw_generation = 0;
		uint64_t origin_revision = 0; // Frame revision of the slots' histories.
		uint64_t drawn_origin_revision = 0; // Frame revision of `drawn`.
		bool ready = false; // The last selection of this view was READY.
	};
	struct RankEntry {
		uint64_t asset_key = 0;
		uint64_t source = 0;
		uint64_t revision = 0;
		uint32_t node_count = 0;
		Vector<uint32_t> rank;
	};
	mutable uint64_t rank_builds = 0;
	mutable Vector<RankEntry> rank_cache; // Shared with the slots; see cached_tie_rank().
	Vector<View> views; // Ascending view key.
	Vector<ViewResult> result_store;
	uint32_t result_count = 0;
	// Scratch, grown (never shrunk) by prepare_scratch(); sorting/deduplication needs no per-call storage.
	mutable Vector<uint32_t> order; // Instance indices by ascending instance key.
	mutable Vector<uint32_t> index_scratch;
	mutable Vector<uint64_t> key_scratch;

	bool cached_tie_rank(const SnapshotAsset &p_asset, Vector<uint32_t> &r_rank) const;
	void prune_rank_cache();
	bool prepare_scratch(const SelectionSnapshot &p_snapshot) const;
	bool validate_snapshot(const SelectionSnapshot &p_snapshot) const;
	bool ledger_impl(const SelectionSnapshot &p_snapshot, Ledger &r_ledger) const;
	void build_order(const SelectionSnapshot &p_snapshot) const;
	static bool required_storage(const SelectionSnapshot &p_snapshot, uint64_t &r_history_bytes, uint64_t &r_nodes);
	bool prepare_slots(View &r_view, const SelectionSnapshot &p_snapshot) const;
};

} // namespace gs_hlod_runtime
#endif
