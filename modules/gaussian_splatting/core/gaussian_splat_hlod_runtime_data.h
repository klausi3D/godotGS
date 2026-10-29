#ifndef GAUSSIAN_SPLAT_HLOD_RUNTIME_DATA_H
#define GAUSSIAN_SPLAT_HLOD_RUNTIME_DATA_H

#include "gaussian_data.h"
#include "gaussian_splat_hlod_tree.h"
#include "gaussian_splat_world_frame.h"
#include "../io/gaussian_splat_world_paged.h"

// Detached, same-generation input to the bounded resident runtime. Publication is by Ref;
// after capture succeeds every payload below is immutable. The source resource is retained,
// but no pointer into its mutable tree or GaussianData storage is borrowed.
class GaussianSplatHlodRuntimeData : public RefCounted {
public:
	static constexpr uint32_t MAX_LEAVES = 1048576;
	static constexpr uint32_t MAX_PAYLOAD = 1572864;
	static constexpr uint32_t MAX_NODES = 32768;
	static constexpr uint64_t CPU_BYTE_LIMIT = uint64_t(1) << 30;
	static constexpr uint64_t GPU_BYTE_LIMIT = uint64_t(512) << 20;

	struct Node {
		GaussianSplatHlodNode topology;
		Vector3 payload_center;
		double exact_cell_center[3] = {};
		// Source-precision cell centre in asset space: tree origin (double) plus the integer lattice address,
		// never reconstructed from payload_center (a rounded float). The per-draw offset reads this.
		gs_world_frame::PreciseCoord precise_center;
		Vector3 bounds_center;
		double footprint = 0.0;
		uint32_t dc_encoding = 0;
		bool homogeneous_dc = true;
	};

	Ref<Resource> source_world;
	Ref<GaussianData> source_data;
	uint64_t source_epoch = 0;
	uint64_t source_revision = 0;
	uint64_t owner_key = 0;
	uint64_t owned_cpu_bytes = 0;
	uint64_t preparation_usec = 0;
	uint32_t leaf_count = 0;
	uint32_t parent_count = 0;
	uint32_t high_order_count = 0;
	uint32_t sh_degree = 0;
	bool mode_2d = false;
	bool antialiased = false;
	bool quantized_eligible = true;
	GaussianData::SaveSnapshot leaves;
	Vector<Node> nodes;
	Vector<Gaussian> parents;
	Vector<Vector3> parent_sh;
	// Package E1: a snapshot of a V4 paged world describes the nodes (topology, footprint, DC metrics) from the
	// file's index and holds NO payload: `leaves`, `parents` and `parent_sh` stay empty and a node's splats are
	// read on demand through `paged_source`. gaussian()/sh() are only valid when !is_paged().
	Ref<GaussianSplatHlodPagedSource> paged_source;
	uint32_t paged_sh_first_order = 0;

	// Package F1 (editable regions, ADR 14.5): an edited snapshot is a NEW immutable snapshot derived from `base` by
	// lod/hlod_edit.*; the base, the world and a V4 package it may read are never modified. Edited leaves keep their
	// node slot (stable NodeKey) and carry their replacement payload in memory (`edited`), in the snapshot's own leaf
	// convention (absolute float32 positions). Every ancestor of an edited leaf is STALE: its baked payload no longer
	// represents its children, so the selector must never draw it or use it as a fallback until a rebake (package F2)
	// replaces it. edit_revision is 0 for an unedited snapshot and rises by exactly one per accepted edit.
	enum EditState : uint8_t { EDIT_CLEAN = 0, EDIT_LEAF = 1, EDIT_STALE = 2 };
	struct EditedLeaf {
		uint32_t node = 0;
		uint64_t revision = 0; // The edit that last replaced this leaf's payload.
		Vector<Gaussian> gaussians; // payload_count entries, leaf convention (absolute float32 positions).
		Vector<Vector3> sh; // payload_count * high_order_count entries (empty when there is no high-order SH).
	};
	uint64_t edit_revision = 0;
	Vector<uint8_t> edit_state; // Empty (no edits) or one EditState per node.
	Vector<EditedLeaf> edited; // Sorted by node.

	bool has_edits() const { return edit_revision != 0 && !edit_state.is_empty(); }
	EditState node_edit_state(uint32_t p_node) const { return p_node < uint32_t(edit_state.size()) ? EditState(edit_state[p_node]) : EDIT_CLEAN; }
	bool is_stale(uint32_t p_node) const { return node_edit_state(p_node) == EDIT_STALE; }
	// The replacement payload of an edited leaf, or null.
	const EditedLeaf *find_edited(uint32_t p_node) const;
	// Content identity of one node for NodeVersion: the source revision for an untouched node, a value that also
	// carries the edit revision for an edited leaf (the high bit separates the two spaces).
	uint64_t node_content_revision(uint32_t p_node) const;

	bool is_paged() const { return paged_source.is_valid(); }
	uint32_t sh_first_order_count() const { return is_paged() ? paged_sh_first_order : leaves.get_sh_first_order_count(); }
	bool is_current() const;
	const Gaussian &gaussian(uint64_t p_index) const;
	const Vector3 *sh(uint64_t p_index) const;
};

namespace gs_hlod_runtime {
// Counts the actual CowData rounded allocation (including its two uint64 headers).
// This also admits simulated narrower size_t limits without attempting huge allocations.
bool allocation_bytes(uint64_t p_count, uint64_t p_stride, uint64_t &r_bytes,
		uint64_t p_size_limit = SIZE_MAX);
}

#endif
