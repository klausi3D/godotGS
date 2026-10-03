#ifndef GAUSSIAN_SPLAT_HLOD_TREE_H
#define GAUSSIAN_SPLAT_HLOD_TREE_H

// Hierarchical LOD tree of a GaussianSplatWorld (ADR docs/architecture/adr-hlod-streaming.md,
// slice S1a: format + bake). Nothing in the renderer reads the tree yet (S2 adds the cut).
//
// World lattice (§6.1, decision 5; ADR head 74bf3699702). One global lattice fixed by the world
// frame: origin O (double; (0, 0, 0) unless the world sets one) and power-of-two cells. A cell is
// (e, ix, iy, iz): edge 2^e metres (e signed, >= -16), signed 64-bit indices, corner
// O + (ix, iy, iz) x 2^e, exact in double. Cell boundaries never depend on the data, so two
// worlds, or a world before and after an edit, agree on every boundary.
//
// Origin-centred root (implementation rule, reported to the ADR author): the planes through O
// are boundaries at every scale, so content that straddles them (most scans are centred on the
// origin) lies in no single lattice cell. The root is then the cube [O - 2^e, O + 2^e) with the
// flag kNodeFlagOriginCentredRoot and address (e, 0, 0, 0); its children are the lattice cells
// (e, {-1, 0}^3), i.e. ordinary cells. Otherwise the root is the smallest lattice cell holding
// every centre.
//
// Positions (§6.2, decision 6). Every payload splat position, and every node AABB, is float32
// relative to the CENTRE of its node's cell. Interior payloads are kept that way in memory.
// The leaf payload of a loaded or baked world is ALSO handed to today's runtime as absolute
// float32 (GaussianData, or the file-backed source with per-leaf frames), so a renderer that
// knows nothing of the tree draws exactly the original splats until S2.
//
// Payload index space. [0, leaf_splat_count) are the original splats, reordered so that every
// leaf is contiguous (leaves in depth-first order, so a subtree's leaves are contiguous too) and
// importance-ordered; [leaf_splat_count, total) are interior payloads, root first in node-table
// (breadth-first) order, each importance-ordered.

#include "core/math/aabb.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"

#include "gaussian_data.h"

#include <cstdint>

namespace gs_hlod {

// Every node payload is at most 16 #1088 pages of 1,024 splats (§6.1, §6.2).
static constexpr uint32_t kMaxNodeSplats = 16384u;
static constexpr uint32_t kMaxChildren = 8u;
static constexpr uint32_t kNoParent = 0xFFFFFFFFu;
// Subdivision stops at the absolute lattice level 2^-16 m (~15 um, §6.1).
static constexpr int32_t kMinCellExponent = -16;
// 2^62 m is far beyond any world a float64 origin can address.
static constexpr int32_t kMaxCellExponent = 62;
// |index| stays below 2^52 so a cell centre O + (i + 0.5) 2^e is exact in double.
static constexpr int64_t kMaxCellIndexMagnitude = int64_t(1) << 52;
// Heights are bounded by the lattice levels between the coarsest and finest cell (78) plus the
// index-split groups under a finest cell (at most 6 levels for 2^32 splats); fails closed above.
static constexpr uint32_t kMaxHeight = 128u;
// Version of the bake RULES (grid, merge, ordering, and from S1b the per-node choice). Not a
// format version: a later rule re-bakes on import without changing the layout (§7).
static constexpr uint32_t kBakeRuleVersion = 1u;

// Representation kind, low two bits of GaussianSplatHlodNode::flags (§6.3, §7).
enum NodeKind : uint32_t {
	NODE_KIND_LEAF = 0u, // original splats
	NODE_KIND_MERGED = 1u, // `sat` moment-matched merge of the children
	NODE_KIND_SELECTED = 2u, // importance selection (written from S1b)
	NODE_KIND_RESERVED = 3u,
};
static constexpr uint32_t kNodeFlagKindMask = 0x3u;
// The node is one of an index-split group: >16,384 centres in a cell at s_min, or bit-identical
// centres, cut by (Morton code, source index). It shares its parent's cell (§6.1).
static constexpr uint32_t kNodeFlagSplitByIndex = 1u << 2u;
// `grouped_leaf`: the leaf groups several sibling octants that together fit 16,384 splats; it is
// addressed by its parent's cell and its positions are relative to that cell's centre (§6.1).
static constexpr uint32_t kNodeFlagGroupedLeaf = 1u << 3u;
// The root is the origin-centred cube described at the top of this file (root only).
static constexpr uint32_t kNodeFlagOriginCentredRoot = 1u << 4u;
static constexpr uint32_t kNodeFlagKnownMask = kNodeFlagKindMask | kNodeFlagSplitByIndex | kNodeFlagGroupedLeaf | kNodeFlagOriginCentredRoot;

static constexpr uint64_t kNodeRecordBytes = 128u;
static constexpr uint64_t kInstanceRecordBytes = 128u;

} // namespace gs_hlod

struct GaussianSplatHlodCell {
	int32_t e = 0; // edge = 2^e metres (int8 on disk)
	int64_t ix = 0;
	int64_t iy = 0;
	int64_t iz = 0;

	bool operator==(const GaussianSplatHlodCell &p_other) const {
		return e == p_other.e && ix == p_other.ix && iy == p_other.iy && iz == p_other.iz;
	}
};

// One node (128 B on disk, §7).
struct GaussianSplatHlodNode {
	GaussianSplatHlodCell cell;
	// Tight bounds of the node's own payload (centres +- 3 sigma per axis, after merging and
	// inflation), united with its children's bounds so a parent always contains its children
	// (§6.1 "node bounds come from the node's own payload"). Relative to the cell centre.
	Vector3 aabb_min;
	Vector3 aabb_max;
	// Sphere about the AABB centre that contains every payload splat's 3-sigma extent and every
	// child's sphere.
	float radius = 0.0f;
	// World-space size of the merge cell (`eps`, §6.3). 0 for leaves; monotone up the tree.
	float geometric_error = 0.0f;
	// Bake-time image error E of the kept and the rejected candidate (§6.3). 0 until S1b.
	float error_chosen = 0.0f;
	float error_rejected = 0.0f;
	uint32_t parent = gs_hlod::kNoParent;
	uint32_t first_child = 0u;
	uint32_t child_count = 0u;
	uint32_t height = 0u;
	uint64_t payload_first = 0u;
	uint32_t payload_count = 0u;
	uint32_t flags = 0u;
	// Per-node overlap footprint (§6.5); reserved in S1a, filled by S2a. Written as 0.
	float overlap_footprint = 0.0f;

	gs_hlod::NodeKind get_kind() const { return gs_hlod::NodeKind(flags & gs_hlod::kNodeFlagKindMask); }
	bool is_leaf() const { return child_count == 0u; }
};

// A placed instance of another asset's tree (§6.9, decision 7). Stage 1 defines and validates
// the record; the producer (the global cut's top-level tree) is slice S2b.
struct GaussianSplatHlodInstance {
	uint64_t instance_key = 0u; // stable key: asset UID x placement identity, never a RID
	int64_t asset_uid = -1; // ResourceUID::ID of the instanced asset
	double basis[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 }; // row-major
	double origin[3] = { 0.0, 0.0, 0.0 };
	uint32_t top_level_node = 0u; // the top-level leaf it hangs from
};

// Bake statistics, kept for diagnostics and the evidence of slice S1a.
struct GaussianSplatHlodBakeStats {
	double tree_build_ms = 0.0;
	double merge_ms = 0.0;
	double order_ms = 0.0;
	double total_ms = 0.0;
	uint32_t index_split_groups = 0u;
};

struct GaussianSplatHlodTree {
	double origin[3] = { 0.0, 0.0, 0.0 }; // lattice origin O (the world frame's origin)
	uint32_t bake_rule_version = gs_hlod::kBakeRuleVersion;
	LocalVector<GaussianSplatHlodNode> nodes;
	uint32_t leaf_splat_count = 0u;
	uint32_t interior_splat_count = 0u;
	uint32_t sh_high_order_count = 0u;
	// Top-level instance tree (§6.9). Its nodes use the same record with an empty payload.
	LocalVector<GaussianSplatHlodNode> top_level_nodes;
	LocalVector<GaussianSplatHlodInstance> instances;

	GaussianSplatHlodBakeStats bake_stats;
	bool has_bake_stats = false;

	// Interior payload, positions relative to each node's cell centre: resident (baked here, or
	// loaded resident), or left in the source file and read on demand.
	bool interior_resident = false;
	LocalVector<Gaussian> interior_gaussians;
	LocalVector<Vector3> interior_sh_high_order;
	String interior_file_path;
	uint64_t interior_gaussian_offset = 0u;
	uint64_t interior_sh_offset = 0u;

	bool is_empty() const { return nodes.is_empty(); }
	void clear();

	// Edge and centre of a node's cell; the origin-centred root is the cube [O - 2^e, O + 2^e).
	double node_cell_edge(const GaussianSplatHlodNode &p_node) const;
	void node_cell_center(const GaussianSplatHlodNode &p_node, double r_center[3]) const;
	void cell_center(const GaussianSplatHlodCell &p_cell, double r_center[3]) const;
	// The node's bounds in world space (rounded to float32).
	AABB node_world_aabb(const GaussianSplatHlodNode &p_node) const;

	// Copies the interior payload (interior_splat_count splats, node-relative positions) from
	// memory or from the source file.
	Error read_interior_payload(LocalVector<Gaussian> &r_gaussians, LocalVector<Vector3> &r_sh_high_order) const;

	Dictionary get_info() const;
};

// Validates a tree against the payload it indexes. Fails closed: returns false and sets r_reason
// on the first violated invariant (§7; persistence is R3).
// - lattice: finite origin; every cell has e in [-16, 62] and |index| < 2^52;
// - shape: node 0 is the root; every other node's parent precedes it; children are contiguous,
//   follow their parent, point back to it and number at most 8; the child ranges cover every
//   non-root node exactly once (one tree, no cycles, no orphans);
// - cells: a child is an octant of its parent's cell (e - 1, index = 2 x parent + bit), one of
//   the origin-centred root's cells (e, {-1, 0}), or, with split_by_index / grouped_leaf, the
//   parent's own cell; only the root may be origin-centred;
// - payload: counts in [1, 16384]; leaf payloads tile [0, leaf_count) and interior payloads tile
//   [leaf_count, leaf_count + interior_count) without gaps or overlap;
// - geometry: finite fields, aabb_min <= aabb_max, radius >= 0, each child's world AABB inside its
//   parent's (to float32 rounding of the cell offsets); geometric error 0 on leaves, > 0 inside,
//   monotone up the tree;
// - kinds and flags: leaf <=> no children <=> LEAF kind; interior nodes MERGED or SELECTED; no
//   unknown flag bits; grouped_leaf only on leaves; heights consistent;
// - instances: every instance references a top-level LEAF; the top-level tree has the same shape
//   rules with empty payloads; transforms finite.
bool gs_hlod_validate_tree(const GaussianSplatHlodTree &p_tree, String *r_reason);

#endif // GAUSSIAN_SPLAT_HLOD_TREE_H
