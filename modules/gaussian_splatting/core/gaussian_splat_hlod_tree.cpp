#include "gaussian_splat_hlod_tree.h"

#include "core/io/file_access.h"
#include "core/math/math_funcs.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

bool _fail(String *r_reason, const String &p_message) {
	if (r_reason) {
		*r_reason = p_message;
	}
	return false;
}

bool _finite3(const Vector3 &p_v) {
	return Math::is_finite(p_v.x) && Math::is_finite(p_v.y) && Math::is_finite(p_v.z);
}

struct PayloadRange {
	uint64_t first = 0u;
	uint64_t count = 0u;
	uint32_t node = 0u;
	bool operator<(const PayloadRange &p_other) const {
		return first < p_other.first || (first == p_other.first && node < p_other.node);
	}
};

// Ranges must tile [p_begin, p_end) exactly: sorted by start, each starting where the previous ended.
bool _tiles(LocalVector<PayloadRange> &p_ranges, uint64_t p_begin, uint64_t p_end, const char *p_what, String *r_reason) {
	std::sort(p_ranges.ptr(), p_ranges.ptr() + p_ranges.size());
	uint64_t cursor = p_begin;
	for (uint32_t i = 0; i < p_ranges.size(); i++) {
		if (p_ranges[i].first != cursor) {
			return _fail(r_reason, vformat("%s payload of node %d starts at %s, expected %s (gap or overlap).",
										   p_what, p_ranges[i].node, String::num_uint64(p_ranges[i].first), String::num_uint64(cursor)));
		}
		cursor += p_ranges[i].count;
	}
	if (cursor != p_end) {
		return _fail(r_reason, vformat("%s payloads cover [%s, %s), expected [%s, %s).", p_what,
									   String::num_uint64(p_begin), String::num_uint64(cursor), String::num_uint64(p_begin), String::num_uint64(p_end)));
	}
	return true;
}

bool _index_in_range(int64_t p_i) {
	return p_i > -gs_hlod::kMaxCellIndexMagnitude && p_i < gs_hlod::kMaxCellIndexMagnitude;
}

bool _cell_in_range(const GaussianSplatHlodCell &p_cell) {
	return p_cell.e >= gs_hlod::kMinCellExponent && p_cell.e <= gs_hlod::kMaxCellExponent && _index_in_range(p_cell.ix) &&
			_index_in_range(p_cell.iy) && _index_in_range(p_cell.iz);
}

// floor(p_i / 2) for signed indices (the parent index of a child cell).
int64_t _floor_half(int64_t p_i) {
	return p_i >= 0 ? p_i / 2 : -((-p_i + 1) / 2);
}

bool _adjacent_to_origin(int64_t p_i) {
	return p_i == -1 || p_i == 0;
}

// Is p_child's cell a legal child of p_parent's cell?
bool _cell_is_child(const GaussianSplatHlodNode &p_parent, const GaussianSplatHlodNode &p_child) {
	const GaussianSplatHlodCell &pc = p_parent.cell;
	const GaussianSplatHlodCell &cc = p_child.cell;
	if ((p_child.flags & (gs_hlod::kNodeFlagSplitByIndex | gs_hlod::kNodeFlagGroupedLeaf)) != 0u) {
		// Shared cell. Under the origin-centred root the shared cell is the root's cube, which
		// has no lattice address; such children carry the root's address AND its flag, so the
		// flag alone says which frame a node uses (one encoding per tree).
		const bool parent_centred = (p_parent.flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u;
		const bool child_centred = (p_child.flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u;
		return cc == pc && parent_centred == child_centred;
	}
	if ((p_parent.flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u) {
		return cc.e == pc.e && _adjacent_to_origin(cc.ix) && _adjacent_to_origin(cc.iy) && _adjacent_to_origin(cc.iz);
	}
	return cc.e == pc.e - 1 && _floor_half(cc.ix) == pc.ix && _floor_half(cc.iy) == pc.iy && _floor_half(cc.iz) == pc.iz;
}

// Shape rules shared by the payload tree and the top-level instance tree.
bool _validate_shape(const GaussianSplatHlodTree &p_tree, const LocalVector<GaussianSplatHlodNode> &p_nodes,
		bool p_top_level, String *r_reason) {
	const char *what = p_top_level ? "top-level node" : "HLOD node";
	const uint64_t node_count = p_nodes.size();
	if (p_nodes[0].parent != gs_hlod::kNoParent) {
		return _fail(r_reason, vformat("%s 0 is not a root.", what));
	}
	if ((p_nodes[0].flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u &&
			(p_nodes[0].cell.ix != 0 || p_nodes[0].cell.iy != 0 || p_nodes[0].cell.iz != 0)) {
		return _fail(r_reason, vformat("%s 0 is origin-centred but not addressed (e, 0, 0, 0).", what));
	}
	if ((p_nodes[0].flags & (gs_hlod::kNodeFlagSplitByIndex | gs_hlod::kNodeFlagGroupedLeaf)) != 0u) {
		return _fail(r_reason, vformat("%s 0 carries a shared-cell flag.", what));
	}
	uint64_t child_slots = 0u;
	for (uint32_t i = 0; i < p_nodes.size(); i++) {
		const GaussianSplatHlodNode &n = p_nodes[i];
		if (i > 0u && !(n.parent < i)) {
			return _fail(r_reason, vformat("%s %d has parent %d, which does not precede it.", what, i, n.parent));
		}
		if (!_cell_in_range(n.cell)) {
			return _fail(r_reason, vformat("%s %d has cell exponent %d or an index outside the lattice range.", what, i, n.cell.e));
		}
		if (i > 0u && (n.flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u) {
			// A shared-cell child of an origin-centred root inherits the flag to name its cube.
			const bool shared = (n.flags & (gs_hlod::kNodeFlagSplitByIndex | gs_hlod::kNodeFlagGroupedLeaf)) != 0u;
			if (!shared || (p_nodes[n.parent].flags & gs_hlod::kNodeFlagOriginCentredRoot) == 0u) {
				return _fail(r_reason, vformat("%s %d is origin-centred but neither the root nor a shared-cell child of it.", what, i));
			}
		}
		if (!_finite3(n.aabb_min) || !_finite3(n.aabb_max) || !Math::is_finite(n.radius) || !Math::is_finite(n.overlap_footprint) ||
				!Math::is_finite(n.geometric_error) || !Math::is_finite(n.error_chosen) || !Math::is_finite(n.error_rejected)) {
			return _fail(r_reason, vformat("%s %d has a non-finite field.", what, i));
		}
		if (n.aabb_min.x > n.aabb_max.x || n.aabb_min.y > n.aabb_max.y || n.aabb_min.z > n.aabb_max.z) {
			return _fail(r_reason, vformat("%s %d has an inverted AABB.", what, i));
		}
		double center[3];
		p_tree.node_cell_center(n, center);
		const double runtime_max = std::numeric_limits<real_t>::max();
		for (int a = 0; a < 3; a++) {
			const double lo = center[a] + double(n.aabb_min[a]);
			const double hi = center[a] + double(n.aabb_max[a]);
			if (!Math::is_finite(center[a]) || std::abs(center[a]) > runtime_max ||
					!Math::is_finite(lo) || !Math::is_finite(hi) || std::abs(lo) > runtime_max || std::abs(hi) > runtime_max) {
				return _fail(r_reason, vformat("%s %d has coordinates outside the finite runtime range.", what, i));
			}
			// node_world_aabb subtracts after narrowing both endpoints to real_t.
			if (!Math::is_finite(real_t(hi) - real_t(lo))) {
				return _fail(r_reason, vformat("%s %d has an AABB size outside the finite runtime range.", what, i));
			}
		}
		if (n.radius < 0.0f || n.error_chosen < 0.0f || n.error_rejected < 0.0f || n.geometric_error < 0.0f || n.overlap_footprint < 0.0f) {
			return _fail(r_reason, vformat("%s %d has a negative radius or error.", what, i));
		}
		// Necessary sphere bound, not an AABB-corner requirement: an isotropic Gaussian's
		// three-sigma sphere is legitimately smaller than its AABB half-diagonal.
		for (int a = 0; a < 3; a++) {
			const double half_extent = 0.5 * (double(n.aabb_max[a]) - double(n.aabb_min[a]));
			if (double(n.radius) < half_extent) {
				return _fail(r_reason, vformat("%s %d radius is smaller than its AABB axis extent.", what, i));
			}
		}
		if ((n.flags & ~gs_hlod::kNodeFlagKnownMask) != 0u) {
			return _fail(r_reason, vformat("%s %d has unknown flag bits 0x%x.", what, i, n.flags));
		}
		if (n.height > gs_hlod::kMaxHeight) {
			return _fail(r_reason, vformat("%s %d has height %d.", what, i, n.height));
		}
		if (n.child_count == 0u) {
			if (n.first_child != 0u || n.height != 0u) {
				return _fail(r_reason, vformat("%s %d is a leaf with a child range or height.", what, i));
			}
			continue;
		}
		if (n.child_count > gs_hlod::kMaxChildren) {
			return _fail(r_reason, vformat("%s %d has %d children (max %d).", what, i, n.child_count, gs_hlod::kMaxChildren));
		}
		if (!(n.first_child > i) || uint64_t(n.first_child) + n.child_count > node_count) {
			return _fail(r_reason, vformat("%s %d child range [%d, +%d) is invalid.", what, i, n.first_child, n.child_count));
		}
		if ((n.flags & gs_hlod::kNodeFlagGroupedLeaf) != 0u) {
			return _fail(r_reason, vformat("%s %d is a grouped leaf but has children.", what, i));
		}
		child_slots += n.child_count;

		double parent_center[3];
		p_tree.node_cell_center(n, parent_center);
		uint32_t tallest = 0u;
		for (uint32_t c = n.first_child; c < n.first_child + n.child_count; c++) {
			const GaussianSplatHlodNode &child = p_nodes[c];
			if (child.parent != i) {
				return _fail(r_reason, vformat("%s %d lists child %d whose parent is %d.", what, i, c, child.parent));
			}
			if (!_cell_is_child(n, child)) {
				return _fail(r_reason, vformat("%s %d: child %d's cell is neither an octant nor (for a split or grouped node) the parent's cell.", what, i, c));
			}
			// World-space containment, evaluated exactly as the bake rounds bounds outward.
			double child_center[3];
			p_tree.node_cell_center(child, child_center);
			for (int a = 0; a < 3; a++) {
				const double child_lo = child_center[a] + double(child.aabb_min[a]);
				const double child_hi = child_center[a] + double(child.aabb_max[a]);
				const double parent_lo = parent_center[a] + double(n.aabb_min[a]);
				const double parent_hi = parent_center[a] + double(n.aabb_max[a]);
				if (child_lo < parent_lo || child_hi > parent_hi) {
					return _fail(r_reason, vformat("%s %d does not contain the bounds of child %d.", what, i, c));
				}
			}
			double distance_squared = 0.0;
			for (int a = 0; a < 3; a++) {
				const double parent_mid = 0.5 * ((parent_center[a] + double(n.aabb_min[a])) + (parent_center[a] + double(n.aabb_max[a])));
				const double child_mid = 0.5 * ((child_center[a] + double(child.aabb_min[a])) + (child_center[a] + double(child.aabb_max[a])));
				const double delta = child_mid - parent_mid;
				distance_squared += delta * delta;
			}
			if (!(double(n.radius) >= std::sqrt(distance_squared) + double(child.radius))) {
				return _fail(r_reason, vformat("%s %d sphere does not contain child %d's sphere.", what, i, c));
			}
			if (!(child.geometric_error <= n.geometric_error)) {
				return _fail(r_reason, vformat("%s geometric error is not monotone at node %d (child %d).", what, i, c));
			}
			tallest = MAX(tallest, child.height);
		}
		if (n.height != tallest + 1u) {
			return _fail(r_reason, vformat("%s %d has height %d, expected %d.", what, i, n.height, tallest + 1u));
		}
	}
	// Every child range points back at its parent, so ranges are disjoint; with this count they
	// cover every non-root node exactly once.
	if (child_slots != node_count - 1u) {
		return _fail(r_reason, vformat("%s child ranges cover %s nodes, expected %s.", what,
									   String::num_uint64(child_slots), String::num_uint64(node_count - 1u)));
	}
	return true;
}

} // namespace

bool gs_hlod_validate_tree(const GaussianSplatHlodTree &p_tree, String *r_reason) {
	for (int a = 0; a < 3; a++) {
		if (!Math::is_finite(p_tree.origin[a])) {
			return _fail(r_reason, "HLOD world origin is not finite.");
		}
	}
	if (p_tree.bake_rule_version == 0u) {
		return _fail(r_reason, "HLOD bake rule version 0 is invalid.");
	}
	const LocalVector<GaussianSplatHlodNode> &nodes = p_tree.nodes;
	const uint64_t node_count = nodes.size();
	if (node_count == 0u) {
		return _fail(r_reason, "HLOD tree has no nodes.");
	}
	const uint64_t leaf_count = p_tree.leaf_splat_count;
	const uint64_t total = leaf_count + uint64_t(p_tree.interior_splat_count);
	// Every node owns at least one splat, so a table larger than the payload is malformed.
	if (node_count > total) {
		return _fail(r_reason, vformat("HLOD tree has %s nodes for %s splats.", String::num_uint64(node_count), String::num_uint64(total)));
	}
	if (!_validate_shape(p_tree, nodes, false, r_reason)) {
		return false;
	}

	LocalVector<PayloadRange> leaf_ranges;
	LocalVector<PayloadRange> interior_ranges;
	for (uint32_t i = 0; i < nodes.size(); i++) {
		const GaussianSplatHlodNode &n = nodes[i];
		if (n.payload_count == 0u || n.payload_count > gs_hlod::kMaxNodeSplats) {
			return _fail(r_reason, vformat("HLOD node %d has payload count %d (allowed 1..%d).", i, n.payload_count, gs_hlod::kMaxNodeSplats));
		}
		if (n.payload_first > total || uint64_t(n.payload_count) > total - n.payload_first) {
			return _fail(r_reason, vformat("HLOD node %d payload range exceeds the payload.", i));
		}
		PayloadRange range;
		range.first = n.payload_first;
		range.count = n.payload_count;
		range.node = i;
		const gs_hlod::NodeKind kind = n.get_kind();
		if (n.child_count == 0u) {
			if (kind != gs_hlod::NODE_KIND_LEAF) {
				return _fail(r_reason, vformat("HLOD node %d has no children but kind %d.", i, int(kind)));
			}
			if (n.geometric_error != 0.0f || n.error_chosen != 0.0f || n.error_rejected != 0.0f) {
				return _fail(r_reason, vformat("HLOD leaf %d has interior-only fields set.", i));
			}
			if (n.payload_first + n.payload_count > leaf_count) {
				return _fail(r_reason, vformat("HLOD leaf %d payload lies outside the leaf range.", i));
			}
			leaf_ranges.push_back(range);
			continue;
		}
		if (kind != gs_hlod::NODE_KIND_MERGED && kind != gs_hlod::NODE_KIND_SELECTED) {
			return _fail(r_reason, vformat("HLOD interior node %d has kind %d.", i, int(kind)));
		}
		if (!(n.geometric_error > 0.0f)) {
			return _fail(r_reason, vformat("HLOD interior node %d has geometric error %f.", i, n.geometric_error));
		}
		if (n.payload_first < leaf_count) {
			return _fail(r_reason, vformat("HLOD interior node %d payload lies inside the leaf range.", i));
		}
		interior_ranges.push_back(range);
	}
	if (!_tiles(leaf_ranges, 0u, leaf_count, "Leaf", r_reason)) {
		return false;
	}
	if (!_tiles(interior_ranges, leaf_count, total, "Interior", r_reason)) {
		return false;
	}

	// Top-level instance tree (§6.9): same shape rules, empty payloads, leaf kind only.
	const LocalVector<GaussianSplatHlodNode> &top = p_tree.top_level_nodes;
	if (top.is_empty() != p_tree.instances.is_empty()) {
		return _fail(r_reason, "HLOD instance table and top-level tree must be both present or both absent.");
	}
	if (!top.is_empty()) {
		// At most one leaf per instance plus its ancestors on the lattice levels.
		const uint64_t bound = uint64_t(p_tree.instances.size()) * uint64_t(gs_hlod::kMaxHeight + 1u) + 1u;
		if (top.size() > bound) {
			return _fail(r_reason, "HLOD top-level tree is larger than its instances can need.");
		}
		if (!_validate_shape(p_tree, top, true, r_reason)) {
			return false;
		}
		for (uint32_t i = 0; i < top.size(); i++) {
			const GaussianSplatHlodNode &n = top[i];
			if (n.payload_count != 0u || n.payload_first != 0u || n.get_kind() != gs_hlod::NODE_KIND_LEAF ||
					n.geometric_error != 0.0f || (n.flags & ~(gs_hlod::kNodeFlagKindMask | gs_hlod::kNodeFlagOriginCentredRoot)) != 0u) {
				return _fail(r_reason, vformat("Top-level node %d carries payload, error or flags (stage 1 has none).", i));
			}
		}
		for (uint32_t i = 0; i < p_tree.instances.size(); i++) {
			const GaussianSplatHlodInstance &inst = p_tree.instances[i];
			if (inst.top_level_node >= top.size() || top[inst.top_level_node].child_count != 0u) {
				return _fail(r_reason, vformat("HLOD instance %d does not reference a top-level leaf.", i));
			}
			for (int k = 0; k < 9; k++) {
				if (!std::isfinite(inst.basis[k])) {
					return _fail(r_reason, vformat("HLOD instance %d has a non-finite transform.", i));
				}
			}
			for (int k = 0; k < 3; k++) {
				if (!std::isfinite(inst.origin[k])) {
					return _fail(r_reason, vformat("HLOD instance %d has a non-finite transform.", i));
				}
			}
		}
	}
	return true;
}

void GaussianSplatHlodTree::clear() {
	*this = GaussianSplatHlodTree();
}

void GaussianSplatHlodTree::cell_center(const GaussianSplatHlodCell &p_cell, double r_center[3]) const {
	const double edge = std::ldexp(1.0, p_cell.e);
	r_center[0] = origin[0] + (double(p_cell.ix) + 0.5) * edge;
	r_center[1] = origin[1] + (double(p_cell.iy) + 0.5) * edge;
	r_center[2] = origin[2] + (double(p_cell.iz) + 0.5) * edge;
}

double GaussianSplatHlodTree::node_cell_edge(const GaussianSplatHlodNode &p_node) const {
	const int extra = (p_node.flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u ? 1 : 0;
	return std::ldexp(1.0, p_node.cell.e + extra);
}

void GaussianSplatHlodTree::node_cell_center(const GaussianSplatHlodNode &p_node, double r_center[3]) const {
	if ((p_node.flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u) {
		r_center[0] = origin[0];
		r_center[1] = origin[1];
		r_center[2] = origin[2];
		return;
	}
	cell_center(p_node.cell, r_center);
}

AABB GaussianSplatHlodTree::node_world_aabb(const GaussianSplatHlodNode &p_node) const {
	double c[3];
	node_cell_center(p_node, c);
	const Vector3 lo(real_t(c[0] + p_node.aabb_min.x), real_t(c[1] + p_node.aabb_min.y), real_t(c[2] + p_node.aabb_min.z));
	const Vector3 hi(real_t(c[0] + p_node.aabb_max.x), real_t(c[1] + p_node.aabb_max.y), real_t(c[2] + p_node.aabb_max.z));
	return AABB(lo, hi - lo);
}

Error GaussianSplatHlodTree::read_interior_payload(LocalVector<Gaussian> &r_gaussians, LocalVector<Vector3> &r_sh_high_order) const {
	const uint64_t sh_count = uint64_t(interior_splat_count) * sh_high_order_count;
	if (interior_resident) {
		ERR_FAIL_COND_V(interior_gaussians.size() != interior_splat_count, ERR_BUG);
		ERR_FAIL_COND_V(uint64_t(interior_sh_high_order.size()) != sh_count, ERR_BUG);
		r_gaussians = interior_gaussians;
		r_sh_high_order = interior_sh_high_order;
		return OK;
	}
	r_gaussians.clear();
	r_sh_high_order.clear();
	if (interior_splat_count == 0u) {
		return OK;
	}
	ERR_FAIL_COND_V(interior_file_path.is_empty(), ERR_UNCONFIGURED);
	Ref<FileAccess> file = FileAccess::open(interior_file_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_FILE_CANT_READ, vformat("Cannot reopen %s to read the HLOD interior payload.", interior_file_path));
	const uint64_t gaussian_bytes = uint64_t(interior_splat_count) * sizeof(Gaussian);
	const uint64_t sh_bytes = sh_count * sizeof(Vector3);
	const uint64_t len = file->get_length();
	ERR_FAIL_COND_V(interior_gaussian_offset > len || gaussian_bytes > len - interior_gaussian_offset, ERR_FILE_CORRUPT);
	r_gaussians.resize(interior_splat_count);
	file->seek(interior_gaussian_offset);
	ERR_FAIL_COND_V(file->get_buffer(reinterpret_cast<uint8_t *>(r_gaussians.ptr()), gaussian_bytes) != gaussian_bytes, ERR_FILE_CORRUPT);
	if (sh_bytes > 0u) {
		ERR_FAIL_COND_V(sh_count > UINT32_MAX, ERR_FILE_CORRUPT);
		ERR_FAIL_COND_V(interior_sh_offset > len || sh_bytes > len - interior_sh_offset, ERR_FILE_CORRUPT);
		r_sh_high_order.resize(uint32_t(sh_count));
		file->seek(interior_sh_offset);
		ERR_FAIL_COND_V(file->get_buffer(reinterpret_cast<uint8_t *>(r_sh_high_order.ptr()), sh_bytes) != sh_bytes, ERR_FILE_CORRUPT);
	}
	return OK;
}

Dictionary GaussianSplatHlodTree::get_info() const {
	Dictionary info;
	info["has_hlod"] = !nodes.is_empty();
	info["node_count"] = int64_t(nodes.size());
	info["leaf_splat_count"] = int64_t(leaf_splat_count);
	info["interior_splat_count"] = int64_t(interior_splat_count);
	info["bake_rule_version"] = int64_t(bake_rule_version);
	info["root_cell_exponent"] = nodes.is_empty() ? int64_t(0) : int64_t(nodes[0].cell.e);
	info["origin_centred_root"] = !nodes.is_empty() && (nodes[0].flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u;
	info["origin"] = Vector3(real_t(origin[0]), real_t(origin[1]), real_t(origin[2]));
	uint32_t leaves = 0u, merged = 0u, selected = 0u, height = 0u, split = 0u;
	int32_t min_exponent = INT32_MAX;
	for (uint32_t i = 0; i < nodes.size(); i++) {
		switch (nodes[i].get_kind()) {
			case gs_hlod::NODE_KIND_LEAF:
				leaves++;
				break;
			case gs_hlod::NODE_KIND_MERGED:
				merged++;
				break;
			case gs_hlod::NODE_KIND_SELECTED:
				selected++;
				break;
			default:
				break;
		}
		if ((nodes[i].flags & gs_hlod::kNodeFlagSplitByIndex) != 0u) {
			split++;
		}
		height = MAX(height, nodes[i].height);
		min_exponent = MIN(min_exponent, nodes[i].cell.e);
	}
	info["leaf_count"] = int64_t(leaves);
	info["merged_count"] = int64_t(merged);
	info["selected_count"] = int64_t(selected);
	info["split_by_index_nodes"] = int64_t(split);
	info["height"] = int64_t(height);
	info["min_cell_exponent"] = nodes.is_empty() ? int64_t(0) : int64_t(min_exponent);
	info["interior_overhead"] = leaf_splat_count > 0u ? double(interior_splat_count) / double(leaf_splat_count) : 0.0;
	info["instance_count"] = int64_t(instances.size());
	if (has_bake_stats) {
		info["bake_tree_build_ms"] = bake_stats.tree_build_ms;
		info["bake_merge_ms"] = bake_stats.merge_ms;
		info["bake_order_ms"] = bake_stats.order_ms;
		info["bake_total_ms"] = bake_stats.total_ms;
	}
	return info;
}
