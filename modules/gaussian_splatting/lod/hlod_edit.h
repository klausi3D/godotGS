#ifndef GS_HLOD_EDIT_H
#define GS_HLOD_EDIT_H

// ADR docs/architecture/adr-hlod-streaming.md section 14.5 (package F1): editable regions in a baked or paged HLOD
// world.
//
// An edit replaces the splats of one or more LEAF nodes. It never modifies a snapshot, a world or a V4 file: it derives
// a new immutable GaussianSplatHlodRuntimeData whose edited leaves carry their replacement payload in memory (an
// overlay over the base, which keeps serving every unedited node, from RAM or from the V4 file) and whose ancestors
// of those leaves are STALE (GaussianSplatHlodRuntimeData::EDIT_STALE). The selector (lod/hlod_runtime_selection.*)
// never draws a stale node and never uses it as a fallback; it replaces it by its children, so until a rebake
// (package F2) the edited region is drawn from the new leaf versions and the untouched siblings along the path.
//
// F1 edit rule (what is accepted, everything else is refused with a distinct status and changes nothing):
//   - only leaf nodes; at least one and at most kMaxEditedLeaves per transaction; at most 16,384 splats per leaf;
//   - every replacement splat is finite and stays inside the leaf's existing bounds (centre +- 3 sigma inside the AABB
//     and the bounding sphere), so no ancestor bound, radius or geometric error changes;
//   - a leaf may not become empty (removing a leaf changes the topology: deferred);
//   - the caller states the revision it edited from; a snapshot that moved on is refused (expected prior revision).
//
// Threading: pure CPU, no shared state; call from any thread. Positions are absolute float32, the leaf convention.

#include "../core/gaussian_splat_hlod_runtime_data.h"

namespace gs_hlod_runtime {

struct LeafEdit {
	uint32_t node = 0;
	Vector<Gaussian> gaussians; // The complete replacement payload of the leaf (absolute float32 positions).
	Vector<Vector3> sh; // gaussians.size() * high_order_count entries (empty when the world has no high-order SH).
};

enum class EditStatus {
	OK,
	INVALID_BASE, // Null, empty or foreign snapshot.
	STALE_REVISION, // The caller edited from another revision than the base's.
	NO_EDITS,
	TOO_MANY_LEAVES,
	INVALID_NODE, // Out of range or duplicated.
	NOT_A_LEAF,
	EMPTY_LEAF, // Would remove the leaf (deferred: topology change).
	LEAF_TOO_LARGE,
	INVALID_PAYLOAD, // Non-finite field, SH size mismatch, or a splat outside the leaf's bounds.
	CENTER_NOT_EXACT, // The leaf can no longer be reconstructed exactly from its cell centre.
	OVER_CPU_BUDGET,
};

static constexpr uint32_t kMaxEditedLeaves = 64;

const char *edit_status_name(EditStatus p_status);

// Derives the edited snapshot. On any failure r_snapshot is untouched and r_reason names the first violated rule.
EditStatus make_edited_snapshot(const Ref<GaussianSplatHlodRuntimeData> &p_base, uint64_t p_expected_revision,
		const LeafEdit *p_edits, uint32_t p_count, Ref<GaussianSplatHlodRuntimeData> &r_snapshot, String &r_reason);

// Nodes whose payload the transition from `p_from` to `p_to` replaced (edited at a revision above p_from's), in
// ascending node order. `p_to` must be a derivative of `p_from` (same node table size).
bool collect_edited_since(const GaussianSplatHlodRuntimeData &p_from, const GaussianSplatHlodRuntimeData &p_to,
		LocalVector<uint32_t> &r_nodes);

// Re-applies the committed edits of `p_edited` onto a freshly captured snapshot of the SAME world (`p_target`, unedited): a
// route rebuild, a cut toggle that materialized a V4 world into the resident shape, or a re-submission. Requires the same
// world object and the same tree (cells, links, payload ranges; an edited leaf is matched by its base range); false and
// nothing written otherwise. The result serves the overlay on top of the target's own payload.
bool rebase_edited_snapshot(const Ref<GaussianSplatHlodRuntimeData> &p_edited, const Ref<GaussianSplatHlodRuntimeData> &p_target,
		Ref<GaussianSplatHlodRuntimeData> &r_snapshot);

} // namespace gs_hlod_runtime

#endif
