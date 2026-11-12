#include "gaussian_splat_hlod_tiled_internal.h"

#include "gaussian_splat_hlod_bake_bounds.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace gs_hlod {
namespace tiled_bake {

void finish_metrics(GaussianSplatHlodNode &r_node, const SplatSpan &p_own,
		const GaussianSplatHlodNode *const *p_children, uint32_t p_child_count) {
	// bake_world uses the zero-origin world lattice for every tile and the stitch.
	const GaussianSplatHlodTree frame;
	double center[3];
	frame.node_cell_center(r_node, center);
	const double shift[3] = { r_node.is_leaf() ? 0.0 : center[0],
		r_node.is_leaf() ? 0.0 : center[1], r_node.is_leaf() ? 0.0 : center[2] };
	bake_bounds::BoundsAccumulator bounds;
	bake_bounds::add_payload_bounds(p_own, shift, bounds);
	double child_lo[kMaxChildren][3] = {};
	double child_hi[kMaxChildren][3] = {};
	uint32_t tallest = 0;
	for (uint32_t c = 0; c < p_child_count; ++c) {
		const GaussianSplatHlodNode &child = *p_children[c];
		double child_center[3];
		frame.node_cell_center(child, child_center);
		for (int a = 0; a < 3; ++a) {
			child_lo[c][a] = child_center[a] + double(child.aabb_min[a]);
			child_hi[c][a] = child_center[a] + double(child.aabb_max[a]);
		}
		bounds.add(child_lo[c], child_hi[c]);
		tallest = MAX(tallest, child.height);
	}
	r_node.height = p_child_count > 0u ? tallest + 1u : 0u;
	double lo[3], hi[3];
	for (int a = 0; a < 3; ++a) {
		r_node.aabb_min[a] = bake_bounds::round_down_offset(center[a], bounds.lo[a]);
		r_node.aabb_max[a] = bake_bounds::round_up_offset(center[a], bounds.hi[a]);
		lo[a] = center[a] + double(r_node.aabb_min[a]);
		hi[a] = center[a] + double(r_node.aabb_max[a]);
	}
	const double mid[3] = { 0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), 0.5 * (lo[2] + hi[2]) };
	double radius = bake_bounds::payload_radius(p_own, shift, mid);
	for (int a = 0; a < 3; ++a) {
		radius = MAX(radius, 0.5 * (double(r_node.aabb_max[a]) - double(r_node.aabb_min[a])));
	}
	for (uint32_t c = 0; c < p_child_count; ++c) {
		const double cmid[3] = { 0.5 * (child_lo[c][0] + child_hi[c][0]),
			0.5 * (child_lo[c][1] + child_hi[c][1]), 0.5 * (child_lo[c][2] + child_hi[c][2]) };
		const double d[3] = { cmid[0] - mid[0], cmid[1] - mid[1], cmid[2] - mid[2] };
		radius = MAX(radius, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + double(p_children[c]->radius));
	}
	r_node.radius = bake_bounds::round_up(radius);
}

namespace {

SplatSpan tile_payload(const BakeResult &p_tile, const GaussianSplatHlodNode &p_node, uint32_t p_high) {
	SplatSpan span;
	span.sh_high_order_count = p_high;
	span.count = p_node.payload_count;
	const bool leaf = p_node.is_leaf();
	if (!leaf && p_node.payload_first < p_tile.tree.leaf_splat_count) {
		return SplatSpan();
	}
	const uint64_t first = leaf ? p_node.payload_first : p_node.payload_first - p_tile.tree.leaf_splat_count;
	const LocalVector<Gaussian> &gaussians = leaf ? p_tile.leaf_gaussians : p_tile.tree.interior_gaussians;
	const LocalVector<Vector3> &sh = leaf ? p_tile.leaf_sh_high_order : p_tile.tree.interior_sh_high_order;
	if (first > gaussians.size() || span.count > uint64_t(gaussians.size()) - first ||
			(first + span.count) * uint64_t(p_high) > sh.size() || span.count == 0u) {
		return SplatSpan();
	}
	span.gaussians = gaussians.ptr() + first;
	span.sh_high_order = p_high > 0u ? sh.ptr() + first * p_high : nullptr;
	return span;
}

struct NodeRef {
	uint32_t plan = 0;
	uint32_t local = kNoParent; // kNoParent means a stitched node, otherwise a tile node
};

const GaussianSplatHlodNode &referenced_record(const Context &p_context, const NodeRef &p_ref) {
	const PlanNode &plan = p_context.plan[p_ref.plan];
	return p_ref.local == kNoParent || p_ref.local == 0u ? plan.record : plan.tile.tree.nodes[p_ref.local];
}

Error stitch(Context &p_context) {
	const GaussianSplatHlodTree frame;
	const InteriorBakeParams params;
	for (uint32_t reverse = p_context.plan.size(); reverse > 0u; --reverse) {
		if (p_context.check() != OK) {
			return p_context.check();
		}
		const uint32_t id = reverse - 1u;
		PlanNode &plan = p_context.plan[id];
		if (plan.work) {
			continue;
		}
		GaussianSplatHlodNode &node = plan.record;
		if (node.child_count == 0u || node.child_count > kMaxChildren) {
			return p_context.fail(ERR_INVALID_DATA);
		}
		double center[3];
		frame.node_cell_center(node, center);
		uint64_t input_count = 0;
		for (uint32_t c = 0; c < node.child_count; ++c) {
			if (plan.children[c] <= id || plan.children[c] >= p_context.plan.size()) {
				return p_context.fail(ERR_INVALID_DATA);
			}
			input_count += p_context.plan[plan.children[c]].record.payload_count;
		}
		// Preallocate both concatenations exactly, so reference.append in
		// bake_interior_node cannot grow either payload owner. The one loaded
		// child dies before the reference allocation. Fresh merge/result payloads
		// are each <= candidate; 3*candidate also covers allocation probes. The
		// 128-byte input term covers cells (24), entries (16), pass flags (1),
		// member indices (4), old+new growth/probes; 64*candidate covers sorting.
		const uint64_t candidate = MIN((input_count + 3u) / 4u, uint64_t(kMaxNodeSplats));
		const uint64_t bytes = (2u * input_count + 3u * candidate) * p_context.payload_bytes +
				input_count * 128u + candidate * 64u + (uint64_t(1) << 20u);
		if (!p_context.reserve(bytes)) {
			return p_context.fail(ERR_OUT_OF_MEMORY);
		}
		struct Release {
			Context &context;
			uint64_t bytes;
			~Release() { context.release(bytes); }
		} release{ p_context, bytes };
		InteriorBakeScratch scratch;
		SplatList children_frame;
		children_frame.sh_high_order_count = p_context.info.sh_high_order_count;
		if (!fallible_resize(children_frame.gaussians, input_count, "gs_hlod::tiled stitch frame") ||
				!fallible_resize(children_frame.sh_high_order, input_count * p_context.info.sh_high_order_count, "gs_hlod::tiled stitch frame SH")) {
			return p_context.fail(ERR_OUT_OF_MEMORY);
		}
		children_frame.clear();
		const GaussianSplatHlodNode *children[kMaxChildren] = {};
		SplatSpan spans[kMaxChildren];
		uint32_t counts[kMaxChildren] = {};
		double max_child_error = 0.0;
		for (uint32_t c = 0; c < node.child_count; ++c) {
			const uint32_t child_id = plan.children[c];
			if (child_id <= id || child_id >= p_context.plan.size()) {
				return p_context.fail(ERR_INVALID_DATA);
			}
			const PlanNode &child_plan = p_context.plan[child_id];
			const GaussianSplatHlodNode &child = child_plan.record;
			children[c] = &child;
			max_child_error = MAX(max_child_error, double(child.geometric_error));
			TiledBakeChunk loaded;
			const Error loaded_error = p_context.sink.load_frontier(child_id, loaded);
			if (loaded_error != OK) {
				return p_context.fail(loaded_error);
			}
			const SplatSpan src = { loaded.gaussians.ptr(), loaded.sh_high_order.ptr(),
				loaded.gaussians.size(), p_context.info.sh_high_order_count };
			if (loaded.sh_high_order.size() != uint64_t(src.count) * src.sh_high_order_count) {
				return p_context.fail(ERR_INVALID_DATA);
			}
			if (src.count == 0u || src.count > kMaxNodeSplats || src.count != child.payload_count) {
				return p_context.fail(ERR_INVALID_DATA);
			}
			counts[c] = src.count;
			double shift[3];
			if (child.is_leaf()) {
				for (int a = 0; a < 3; ++a) {
					shift[a] = -center[a];
				}
			} else {
				double child_center[3];
				frame.node_cell_center(child, child_center);
				for (int a = 0; a < 3; ++a) {
					shift[a] = child_center[a] - center[a];
				}
			}
			const uint32_t first = children_frame.size();
			const Error appended = children_frame.append(src, 0u, src.count);
			if (appended != OK) {
				return p_context.fail(appended == ERR_OUT_OF_MEMORY ? appended : ERR_INVALID_DATA);
			}
			for (uint32_t k = first; k < children_frame.size(); ++k) {
				Vector3 &p = children_frame.gaussians[k].position;
				p = Vector3(real_t(double(p.x) + shift[0]), real_t(double(p.y) + shift[1]), real_t(double(p.z) + shift[2]));
			}
		}
		uint32_t offset = 0u;
		for (uint32_t c = 0; c < node.child_count; ++c) {
			spans[c].gaussians = children_frame.gaussians.ptr() + offset;
			spans[c].sh_high_order = p_context.info.sh_high_order_count > 0u ?
					children_frame.sh_high_order.ptr() + uint64_t(offset) * p_context.info.sh_high_order_count : nullptr;
			spans[c].count = counts[c];
			spans[c].sh_high_order_count = p_context.info.sh_high_order_count;
			offset += counts[c];
		}
		if (!fallible_resize(scratch.reference.gaussians, input_count, "gs_hlod::tiled stitch reference") ||
				!fallible_resize(scratch.reference.sh_high_order, input_count * p_context.info.sh_high_order_count, "gs_hlod::tiled stitch reference SH")) {
			return p_context.fail(ERR_OUT_OF_MEMORY);
		}
		const double edge = frame.node_cell_edge(node);
		const Vector3 origin(real_t(-0.5 * edge), real_t(-0.5 * edge), real_t(-0.5 * edge));
		InteriorBakeResult result;
		Error code = ERR_INVALID_DATA;
		String reason;
		if (!bake_interior_node(spans, node.child_count, origin, edge, max_child_error,
				params, scratch, result, &reason, &code)) {
			return p_context.fail(code == ERR_OUT_OF_MEMORY ? code : ERR_INVALID_DATA);
		}
		if (p_context.check() != OK) {
			return p_context.check();
		}
		node.geometric_error = bake_bounds::round_up(result.geometric_error);
		for (uint32_t c = 0; c < node.child_count; ++c) {
			node.geometric_error = MAX(node.geometric_error, children[c]->geometric_error);
		}
		node.payload_count = result.payload.size();
		node.flags = (node.flags & ~kNodeFlagKindMask) | NODE_KIND_MERGED;
		finish_metrics(node, result.payload.span(), children, node.child_count);
		const Error emitted = p_context.sink.append_node(id, node, plan.depth, result.payload.span(), nullptr, true);
		if (emitted != OK) {
			return p_context.fail(emitted);
		}
	}
	return p_context.check();
}

} // namespace

Error emit_tile(Context &p_context, uint32_t p_plan) {
	PlanNode &plan = p_context.plan[p_plan];
	BakeResult &tile = plan.tile;
	if (tile.tree.nodes.is_empty() || uint64_t(p_context.total_nodes) + tile.tree.nodes.size() - 1u > UINT32_MAX) {
		return p_context.fail(ERR_INVALID_DATA);
	}
	plan.extra_id_first = p_context.total_nodes;
	const uint32_t total = p_context.total_nodes + tile.tree.nodes.size() - 1u;
	if (!p_context.reserve_metadata(total)) {
		return p_context.fail(ERR_OUT_OF_MEMORY);
	}
	p_context.total_nodes = total;
	LocalVector<uint32_t> depths;
	if (!fallible_resize(depths, tile.tree.nodes.size(), "gs_hlod::tiled emission depths")) {
		return p_context.fail(ERR_OUT_OF_MEMORY);
	}
	for (uint32_t i = 0; i < tile.tree.nodes.size(); ++i) {
		if (p_context.check() != OK) {
			return p_context.check();
		}
		const GaussianSplatHlodNode &node = tile.tree.nodes[i];
		if (i > 0u && node.parent >= i) {
			return p_context.fail(ERR_INVALID_DATA);
		}
		depths[i] = i == 0u ? plan.depth : depths[node.parent] + 1u;
		const SplatSpan payload = tile_payload(tile, node, p_context.info.sh_high_order_count);
		if (payload.count != node.payload_count || (node.is_leaf() &&
				node.payload_first + node.payload_count > tile.leaf_source_index.size())) {
			return p_context.fail(ERR_INVALID_DATA);
		}
		const uint32_t *indices = node.is_leaf() ? tile.leaf_source_index.ptr() + node.payload_first : nullptr;
		const uint32_t provisional = i == 0u ? p_plan : plan.extra_id_first + i - 1u;
		const Error error = p_context.sink.append_node(provisional, node, depths[i], payload, indices, i == 0u);
		if (error != OK) {
			return p_context.fail(error);
		}
	}
	// The coordinator owns these buffers after join. Keep only local topology.
	tile.leaf_gaussians.reset();
	tile.leaf_sh_high_order.reset();
	tile.leaf_source_index.reset();
	tile.tree.interior_gaussians.reset();
	tile.tree.interior_sh_high_order.reset();
	tile.tree.interior_resident = false;
	return p_context.check();
}

Error stitch_and_assemble(Context &p_context) {
	const Error stitched = stitch(p_context);
	if (stitched != OK) {
		return stitched;
	}
	if (p_context.plan.is_empty()) {
		return p_context.fail(ERR_INVALID_DATA);
	}
	uint64_t total_nodes = 0u;
	for (uint32_t i = 0; i < p_context.plan.size(); ++i) {
		if ((i & 4095u) == 0u && p_context.check() != OK) {
			return p_context.check();
		}
		const PlanNode &plan = p_context.plan[i];
		if (plan.work && plan.tile.tree.nodes.is_empty()) {
			return p_context.fail(ERR_INVALID_DATA);
		}
		total_nodes += plan.work ? plan.tile.tree.nodes.size() : 1u;
	}
	if (total_nodes > p_context.node_bound || total_nodes > UINT32_MAX) {
		return p_context.fail(ERR_OUT_OF_MEMORY);
	}
	GaussianSplatHlodTree tree;
	tree.leaf_splat_count = p_context.info.splat_count;
	tree.sh_high_order_count = p_context.info.sh_high_order_count;
	tree.bake_rule_version = kBakeRuleVersion;
	LocalVector<NodeRef> refs;
	if (!fallible_resize(refs, total_nodes, "gs_hlod::tiled assembly refs") ||
			!fallible_resize(tree.nodes, total_nodes, "gs_hlod::tiled assembly nodes")) {
		return p_context.fail(ERR_OUT_OF_MEMORY);
	}
	refs[0] = { 0u, p_context.plan[0].work ? 0u : kNoParent };
	uint32_t queued = 1u;
	uint64_t interior_total = 0u;
	for (uint32_t i = 0; i < queued; ++i) {
		if (p_context.check() != OK) {
			return p_context.check();
		}
		const NodeRef ref = refs[i];
		const GaussianSplatHlodNode &source_node = referenced_record(p_context, ref);
		const uint32_t parent = i == 0u ? kNoParent : tree.nodes[i].parent;
		tree.nodes[i] = source_node;
		GaussianSplatHlodNode &node = tree.nodes[i];
		node.parent = parent;
		if (node.child_count > kMaxChildren || uint64_t(queued) + node.child_count > total_nodes) {
			return p_context.fail(ERR_INVALID_DATA);
		}
		node.first_child = node.is_leaf() ? 0u : queued;
		for (uint32_t c = 0; c < node.child_count; ++c) {
			if (ref.local == kNoParent) {
				const uint32_t plan_id = p_context.plan[ref.plan].children[c];
				if (plan_id >= p_context.plan.size()) {
					return p_context.fail(ERR_INVALID_DATA);
				}
				refs[queued] = { plan_id, p_context.plan[plan_id].work ? 0u : kNoParent };
			} else {
				const uint64_t local = uint64_t(source_node.first_child) + c;
				if (local >= p_context.plan[ref.plan].tile.tree.nodes.size()) {
					return p_context.fail(ERR_INVALID_DATA);
				}
				refs[queued] = { ref.plan, uint32_t(local) };
			}
			tree.nodes[queued].parent = i;
			++queued;
		}
		if (!node.is_leaf()) {
			interior_total += node.payload_count;
		}
	}
	if (queued != total_nodes || interior_total + tree.leaf_splat_count > UINT32_MAX ||
			(interior_total + tree.leaf_splat_count) * uint64_t(tree.sh_high_order_count) > UINT32_MAX) {
		return p_context.fail(ERR_INVALID_DATA);
	}
	tree.interior_splat_count = uint32_t(interior_total);
	LocalVector<uint32_t> stack;
	LocalVector<uint32_t> bfs_to_provisional;
	if (!fallible_resize(stack, total_nodes, "gs_hlod::tiled assembly DFS") ||
			!fallible_resize(bfs_to_provisional, total_nodes, "gs_hlod::tiled assembly IDs")) {
		return p_context.fail(ERR_OUT_OF_MEMORY);
	}
	for (uint32_t i = 0; i < refs.size(); ++i) {
		const NodeRef &ref = refs[i];
		bfs_to_provisional[i] = ref.local == kNoParent || ref.local == 0u ? ref.plan :
				p_context.plan[ref.plan].extra_id_first + ref.local - 1u;
	}
	uint32_t stack_size = 1u;
	stack[0] = 0u;
	uint32_t leaf_cursor = 0u;
	while (stack_size > 0u) {
		if (p_context.check() != OK) {
			return p_context.check();
		}
		const uint32_t id = stack[--stack_size];
		GaussianSplatHlodNode &node = tree.nodes[id];
		if (!node.is_leaf()) {
			if (uint64_t(stack_size) + node.child_count > stack.size()) {
				return p_context.fail(ERR_INVALID_DATA);
			}
			for (uint32_t c = node.child_count; c > 0u; --c) {
				stack[stack_size++] = node.first_child + c - 1u;
			}
			continue;
		}
		const NodeRef ref = refs[id];
		if (ref.local == kNoParent) {
			return p_context.fail(ERR_INVALID_DATA);
		}
		if (node.payload_count == 0u || node.payload_count > tree.leaf_splat_count - leaf_cursor) {
			return p_context.fail(ERR_INVALID_DATA);
		}
		node.payload_first = leaf_cursor;
		leaf_cursor += node.payload_count;
	}
	if (leaf_cursor != tree.leaf_splat_count) {
		return p_context.fail(ERR_INVALID_DATA);
	}
	uint32_t interior_cursor = 0u;
	for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
		if (p_context.check() != OK) {
			return p_context.check();
		}
		GaussianSplatHlodNode &node = tree.nodes[i];
		if (node.is_leaf()) {
			continue;
		}
		if (node.payload_count == 0u || node.payload_count > tree.interior_splat_count - interior_cursor) {
			return p_context.fail(ERR_INVALID_DATA);
		}
		node.payload_first = uint64_t(tree.leaf_splat_count) + interior_cursor;
		interior_cursor += node.payload_count;
	}
	if (interior_cursor != tree.interior_splat_count) {
		return p_context.fail(ERR_INVALID_DATA);
	}
	tree.interior_resident = false;
	String reason;
	Error validation = OK;
	if (!gs_hlod_validate_tree(tree, &reason, &validation)) {
		return p_context.fail(validation == ERR_OUT_OF_MEMORY ? validation : ERR_INVALID_DATA);
	}
	if (p_context.check() != OK) {
		return p_context.check();
	}
	return p_context.sink.finish(tree, bfs_to_provisional);
}

} // namespace tiled_bake
} // namespace gs_hlod
