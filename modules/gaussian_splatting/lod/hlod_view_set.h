#ifndef GS_HLOD_VIEW_SET_H
#define GS_HLOD_VIEW_SET_H

// ADR docs/architecture/adr-hlod-streaming.md sections 14.1, 14.2 and 6.9 (package B2).
//
// CPU-only contracts that sit on top of the existing S2a selector (`CutHistory`); there is no
// second selector. Nothing here is a bound API or a registered class, and the renderer does not
// consume it yet (see the follow-up note in the PR): the renderer still keeps its own four
// per-buffer histories.
//
//   - NodeKey / NodeVersion: stable node identity and content identity, independent of atlas
//     address and of node-array order.
//   - SelectionSnapshot: an immutable input (hierarchy/content revision, transforms, residency,
//     budgets, per-view parameters). Selection only reads it.
//   - ViewSet: each admitted view owns its cut histories and its published draw records.
//     One physical byte budget covers all views and instances: shared payload is charged once per
//     asset, per-view sort/record/draw allocations are charged once per admitted view.
//     Admitting a view either reserves everything it needs or returns an explicit refusal; no
//     other view's caps, history or published draw records are touched by either outcome.
//
// Threading: not thread-safe; call from the thread that owns the selection state.

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
	Transform3D transform; // Rigid plus uniform scale; anything else is refused.
	// This instance's share of every view's caps. Admission requires the sum over instances to fit
	// the view's caps, so a view never exceeds what was reserved for it. A shared cut across
	// instances needs the top-level tree (ADR 6.9, slice S2b) and is deferred.
	Caps caps;
};

struct ViewSpec {
	uint64_t view_key = 0;
	Vector3 camera; // World space.
	Plane frustum[6]; // World space.
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
};

class ViewSet {
public:
	enum AdmitStatus {
		ADMITTED,
		REFUSED_INVALID, // Malformed snapshot or spec.
		REFUSED_DUPLICATE, // The view key is already admitted.
		REFUSED_VIEW_LIMIT,
		REFUSED_CAPS, // Instance caps do not fit the view's caps.
		REFUSED_BUDGET, // Shared payload plus all admitted views plus this one exceed the budget.
		REFUSED_RESOURCES, // A cut workspace or draw storage could not be allocated.
	};
	struct AdmitResult {
		AdmitStatus status = REFUSED_INVALID;
		uint64_t view_bytes = 0; // This view's own sort/record/draw charge.
		uint64_t charged_bytes = 0; // Shared payload plus already-admitted views.
		uint64_t budget_bytes = 0;
	};
	// Semantic readiness (ADR 14.2); not a bound enum.
	enum Readiness {
		LOADING, // Residency for the complete frontier is still missing.
		READY, // A complete coarse representation (every selected node resident) is published.
		NOT_ADMITTED, // The configured budgets/caps cannot hold a complete frontier.
		FAILED, // Invalid input or arithmetic.
	};
	struct ViewResult {
		uint64_t view_key = 0;
		Readiness readiness = FAILED;
		CutHistory::Status worst_status = CutHistory::COMPLETE; // The first non-success instance status.
		uint32_t drawn_count = 0;
		uint64_t selected_splats = 0;
		double estimated_records = 0.0;
		bool budget_bound = false;
		uint64_t draw_generation = 0; // Bumped only when a new complete draw set is published.
		// The snapshot asks for more than the view reserved at admission (or the shared budget no longer
		// holds); nothing was selected or published for this view.
		bool reservation_exceeded = false;
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

	// Runs one selection per admitted view that appears in the snapshot, in ascending view key.
	// A view that cannot be complete reports its state and publishes nothing new; its previous
	// published draw set stays but is only valid for a READY result. Other views are unaffected.
	SnapshotStatus select(const SelectionSnapshot &p_snapshot, Vector<ViewResult> &r_results);

	// Published draw records of an admitted view (nullptr if unknown), ordered by (instance key,
	// NodeKey) and therefore independent of node-array order.
	const DrawRecord *drawn(uint64_t p_view_key, uint32_t &r_count) const;

	// Shared payload (once per distinct asset key) plus the reservations of the admitted views.
	// Returns false on overflow or a malformed snapshot.
	bool ledger(const SelectionSnapshot &p_snapshot, Ledger &r_ledger) const;
	static bool view_cost(const ResidencyBudget &p_budget, const Caps &p_caps, uint64_t &r_bytes);

private:
	struct InstanceSlot {
		uint64_t instance_key = 0;
		uint32_t node_count = 0;
		uint64_t content_revision = 0;
		CutHistory history;
	};
	struct View {
		uint64_t view_key = 0;
		Caps reserved;
		uint64_t reserved_bytes = 0;
		Vector<InstanceSlot> slots;
		Vector<DrawRecord> drawn;
		Vector<DrawRecord> staging;
		uint32_t drawn_count = 0;
		uint64_t draw_generation = 0;
	};
	Vector<View> views; // Ascending view key.

	static bool validate_snapshot(const SelectionSnapshot &p_snapshot);
	bool prepare_slots(View &r_view, const SelectionSnapshot &p_snapshot) const;
};

} // namespace gs_hlod_runtime
#endif
