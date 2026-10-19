#include "hlod_runtime_cut.h"

#ifdef TESTS_ENABLED
#include "core/os/os.h"
#include "core/os/thread.h"
#include "../core/gaussian_splat_hlod_merge.h"
#include "../core/gs_vector_alloc.h"

#include <cmath>
#include <functional>
#include <limits>

namespace gs_hlod::runtime_cut {
namespace {

struct Bounds {
	double lo[3] = {INFINITY, INFINITY, INFINITY};
	double hi[3] = {-INFINITY, -INFINITY, -INFINITY};
	void add(int p_axis, double p_lo, double p_hi)
	{
		lo[p_axis] = MIN(lo[p_axis], p_lo);
		hi[p_axis] = MAX(hi[p_axis], p_hi);
	}
	void add_node(const GaussianSplatHlodTree &p_tree, const GaussianSplatHlodNode &p_node)
	{
		double center[3];
		p_tree.node_cell_center(p_node, center);
		for (int a = 0; a < 3; ++a) {
			add(a, center[a] + double(p_node.aabb_min[a]), center[a] + double(p_node.aabb_max[a]));
		}
	}
	bool finish(AABB &r_box) const
	{
		for (int a = 0; a < 3; ++a) {
			const double limit = std::numeric_limits<real_t>::max();
			if (!std::isfinite(lo[a]) || !std::isfinite(hi[a]) || lo[a] > hi[a] || std::abs(lo[a]) > limit ||
			    std::abs(hi[a]) > limit) {
				return false;
			}
			real_t lower = real_t(lo[a]);
			if (double(lower) > lo[a]) {
				lower = std::nextafter(lower, real_t(-INFINITY));
			}
			// Round the absolute endpoint first. An ULP of a small size can be
			// far smaller than an ULP of position + size, so incrementing only
			// the size need not move the runtime endpoint past the original hi.
			real_t upper = real_t(hi[a]);
			if (double(upper) < hi[a]) {
				upper = std::nextafter(upper, real_t(INFINITY));
			}
			const double extent = double(upper) - double(lower);
			if (!std::isfinite(extent) || extent > limit) {
				return false;
			}
			real_t size = real_t(extent);
			if (double(size) < extent) {
				size = std::nextafter(size, real_t(INFINITY));
			}
			// The target is now itself representable in the runtime scalar type.
			if (double(lower) + double(size) < double(upper) || lower + size < upper) {
				size = std::nextafter(size, real_t(INFINITY));
			}
			if (!Math::is_finite(lower) || !Math::is_finite(size) || !Math::is_finite(lower + size) ||
			    double(lower) + double(size) < hi[a] || lower + size < upper) {
				return false;
			}
			r_box.position[a] = lower;
			r_box.size[a] = size;
		}
		return true;
	}
};

bool finite3(const Vector3 &p_v)
{
	return Math::is_finite(p_v.x) && Math::is_finite(p_v.y) && Math::is_finite(p_v.z);
}

bool payload_valid(const Gaussian &p_g, const Vector3 *p_sh, uint32_t p_high, const GaussianSplatHlodNode &p_node,
                   const double p_center[3], bool p_leaf)
{
	if (!gaussian_render_fields_finite(p_g) || !finite3(p_g.normal) || !Math::is_finite(p_g.area) ||
	    !Math::is_finite(p_g.stroke_age) || !Math::is_finite(p_g.brush_axes.x) || !Math::is_finite(p_g.brush_axes.y)) {
		return false;
	}
	for (uint32_t k = 0; k < p_high; ++k) {
		if (!finite3(p_sh[k])) {
			return false;
		}
	}
	double cov[6];
	splat_covariance(p_g, cov);
	const int diagonal[] = {0, 3, 5};
	for (int a = 0; a < 3; ++a) {
		const double absolute = double(p_g.position[a]) + (p_leaf ? 0.0 : p_center[a]);
		const double support = 3.0 * std::sqrt(MAX(cov[diagonal[a]], 0.0));
		if (!std::isfinite(support) || absolute - support < p_center[a] + double(p_node.aabb_min[a]) ||
		    absolute + support > p_center[a] + double(p_node.aabb_max[a])) {
			return false;
		}
	}
	return true;
}

} // namespace

Result build_cut(const Ref<GaussianSplatWorld> &p_source, const Request &p_request)
{
	const uint64_t started = OS::get_singleton()->get_ticks_usec();
	Result result;
	auto fail = [&](Status p_status, const String &p_reason) {
		Result failure;
		failure.status = p_status;
		failure.reason = p_reason;
		failure.cpu_usec = OS::get_singleton()->get_ticks_usec() - started;
		return failure;
	};
	if (!Thread::is_main_thread() || p_source.is_null()) {
		return fail(Status::INVALID_INPUT, "A quiescent main-thread resident source is required");
	}
	if (p_request.deformation) {
		return fail(Status::UNSUPPORTED, "Deformation is unsupported");
	}
	if (p_request.views.is_empty() || p_request.views.size() > 2 || !std::isfinite(p_request.threshold_pixels) ||
	    p_request.threshold_pixels < 0) {
		return fail(Status::INVALID_INPUT, "Invalid view count or heuristic threshold");
	}
	double directions[2][3] = {};
	for (int v = 0; v < p_request.views.size(); ++v) {
		const View &view = p_request.views[v];
		const double norm =
		        std::sqrt(double(view.forward.x) * view.forward.x + double(view.forward.y) * view.forward.y +
		                  double(view.forward.z) * view.forward.z);
		if (!finite3(view.position) || !finite3(view.forward) || !std::isfinite(norm) || norm == 0 ||
		    !std::isfinite(view.focal_pixels) || view.focal_pixels <= 0 || !std::isfinite(view.near_plane) ||
		    view.near_plane <= 0) {
			return fail(Status::INVALID_INPUT, "Invalid camera/projection input");
		}
		for (int a = 0; a < 3; ++a) {
			directions[v][a] = double(view.forward[a]) / norm;
		}
	}
	const Ref<GaussianData> data = p_source->get_gaussian_data();
	const GaussianSplatHlodTree &tree = p_source->get_hlod_tree();
	if (!tree.instances.is_empty() || !tree.top_level_nodes.is_empty()) {
		return fail(Status::UNSUPPORTED, "Top-level instances are unsupported");
	}
	if (data.is_null()) {
		return fail(Status::UNAVAILABLE, "Resident leaf payload required");
	}
	// These view states do not bump the base payload revision. Inspect the
	// actual source under the documented quiescent main-thread precondition.
	if (data->has_animation() || data->has_modifications()) {
		return fail(Status::UNSUPPORTED, "Source animation and runtime overlays are unsupported");
	}
	uint32_t leaf_count = 0, high = 0;
	if (data->capture_save_layout(leaf_count, high) != OK) {
		return fail(Status::INVALID_INPUT, "Invalid resident payload layout");
	}
	if (leaf_count == 0 || tree.nodes.is_empty()) {
		return fail(Status::EMPTY_INPUT, "No baked resident tree");
	}
	const uint64_t total = uint64_t(leaf_count) + tree.interior_splat_count;
	if (tree.nodes.size() > MAX_NODES || total > MAX_INPUT_SPLATS || high > 21 || tree.leaf_splat_count != leaf_count ||
	    tree.sh_high_order_count != high) {
		return fail(Status::INVALID_INPUT, "Input admission bound/layout exceeded");
	}
	const uint64_t admitted =
	        4u * total * (sizeof(Gaussian) + uint64_t(high) * sizeof(Vector3)) + uint64_t(tree.nodes.size()) * 1024u;
	if (p_request.allocation_budget_bytes > MAX_ALLOCATION_BYTES || admitted > p_request.allocation_budget_bytes) {
		return fail(Status::ALLOCATION_FAILED, "Allocation admission budget exceeded");
	}
	if (tree.bake_rule_version != kBakeRuleVersion) {
		return fail(Status::UNSUPPORTED, "Unsupported bake generation");
	}
	if (!p_source->is_hlod_payload_current() || data->get_content_revision() != p_request.source_revision) {
		return fail(Status::STALE, "Source revision changed");
	}
	if (!p_request.payload_available.is_empty() && p_request.payload_available.size() != tree.nodes.size()) {
		return fail(Status::INVALID_INPUT, "Availability table size mismatch");
	}
	for (uint8_t available : p_request.payload_available) {
		if (available > 1) {
			return fail(Status::INVALID_INPUT, "Invalid availability value");
		}
	}
	// The existing validator allocates range vectors. Bound and probe before calling it;
	// this follows the bake's existing fallible-probe convention, not an OOM guarantee.
	if (!bake_allocation_probe(uint64_t(tree.nodes.size()) * 128u, "HLODCut topology")) {
		return fail(Status::ALLOCATION_FAILED, "Topology allocation probe failed");
	}
	String reason;
	if (!gs_hlod_validate_tree(tree, &reason)) {
		return fail(Status::INVALID_INPUT, reason);
	}
	if (tree.interior_resident && (tree.interior_gaussians.size() != tree.interior_splat_count ||
	                               tree.interior_sh_high_order.size() != uint64_t(tree.interior_splat_count) * high)) {
		return fail(Status::INVALID_INPUT, "Resident interior payload layout mismatch");
	}
	GaussianData::SaveSnapshot snapshot;
	if (data->capture_save_snapshot(snapshot) != OK) {
		return fail(Status::ALLOCATION_FAILED, "Leaf snapshot failed");
	}
	const uint32_t degree = snapshot.get_sh_degree();
	if (snapshot.get_content_revision() != p_request.source_revision) {
		return fail(Status::STALE, "Snapshot revision changed");
	}
	if (degree > 4 || snapshot.get_sh_first_order_count() != (degree == 0 ? 0u : 3u) ||
	    high != (degree < 2 ? 0u : (degree + 1u) * (degree + 1u) - 4u)) {
		return fail(Status::INVALID_INPUT, "Only complete SH0-4 layouts are admitted");
	}
	auto available = [&](uint32_t p_id) {
		return (tree.nodes[p_id].is_leaf() || tree.interior_resident) &&
		       (p_request.payload_available.is_empty() || p_request.payload_available[p_id] != 0);
	};
	auto payload = [&](const GaussianSplatHlodNode &p_node) -> const Gaussian * {
		return p_node.is_leaf() ? snapshot.get_gaussians().ptr() + p_node.payload_first
		                        : tree.interior_gaussians.ptr() + (p_node.payload_first - leaf_count);
	};
	auto sh_payload = [&](const GaussianSplatHlodNode &p_node) -> const Vector3 * {
		if (high == 0) {
			return nullptr;
		}
		return p_node.is_leaf() ? snapshot.get_sh_high_order().ptr() + p_node.payload_first * high
		                        : tree.interior_sh_high_order.ptr() + (p_node.payload_first - leaf_count) * high;
	};
	for (uint32_t id = 0; id < tree.nodes.size(); ++id) {
		if (!available(id)) {
			continue;
		}
		const auto &node = tree.nodes[id];
		double center[3];
		tree.node_cell_center(node, center);
		const Gaussian *g = payload(node);
		const Vector3 *sh = sh_payload(node);
		for (uint32_t i = 0; i < node.payload_count; ++i) {
			if (!payload_valid(g[i], high ? sh + uint64_t(i) * high : nullptr, high, node, center, node.is_leaf())) {
				return fail(Status::INVALID_INPUT, "Available payload is nonfinite or outside its node bounds");
			}
		}
	}
	Vector<uint64_t> minimum;
	if (!gs_resize_or_fail(minimum, tree.nodes.size(), "HLODCut minimum") ||
	    !gs_resize_or_fail(result.selected_nodes, tree.nodes.size(), "HLODCut selected")) {
		return fail(Status::ALLOCATION_FAILED, "Cut allocation failed");
	}
	constexpr uint64_t UNAVAILABLE = UINT64_MAX / 2u;
	for (int64_t id = int64_t(tree.nodes.size()) - 1; id >= 0; --id) {
		const auto &node = tree.nodes[id];
		uint64_t children = 0;
		for (uint32_t c = node.first_child; c < node.first_child + node.child_count; ++c) {
			children = MIN(UNAVAILABLE, children + minimum[c]);
		}
		minimum.write[id] = MIN(available(id) ? uint64_t(node.payload_count) : UNAVAILABLE,
		                        node.is_leaf() ? UNAVAILABLE : children);
	}
	if (minimum[0] == UNAVAILABLE) {
		return fail(Status::UNAVAILABLE, "No complete available cut");
	}
	if (minimum[0] > p_request.splat_budget) {
		return fail(Status::BUDGET_INFEASIBLE, "No complete available cut fits selected-splat budget");
	}
	auto heuristic = [&](uint32_t p_id, bool &r_available) {
		const auto &node = tree.nodes[p_id];
		Bounds bounds;
		bounds.add_node(tree, node);
		r_available = true;
		bool satisfied = true;
		for (int v = 0; v < p_request.views.size(); ++v) {
			double depth = 0;
			for (int a = 0; a < 3; ++a) {
				depth += ((directions[v][a] >= 0 ? bounds.lo[a] : bounds.hi[a]) -
				          double(p_request.views[v].position[a])) *
				         directions[v][a];
			}
			if (!std::isfinite(depth) || depth <= p_request.views[v].near_plane) {
				r_available = false;
				satisfied = false;
				continue;
			}
			const double pixels = double(node.geometric_error) / depth * p_request.views[v].focal_pixels;
			if (!std::isfinite(pixels)) {
				r_available = false;
				satisfied = false;
			} else {
				satisfied &= pixels <= p_request.threshold_pixels;
			}
		}
		return satisfied && (!p_request.force_leaves || node.is_leaf());
	};
	uint32_t selected_count = 0;
	std::function<uint64_t(uint32_t, uint64_t)> select = [&](uint32_t p_id, uint64_t p_budget) -> uint64_t {
		const auto &node = tree.nodes[p_id];
		bool known;
		const bool wanted = heuristic(p_id, known);
		uint64_t children_min = 0;
		for (uint32_t c = node.first_child; c < node.first_child + node.child_count; ++c) {
			children_min = MIN(UNAVAILABLE, children_min + minimum[c]);
		}
		if (available(p_id) && node.payload_count <= p_budget &&
		    (node.is_leaf() || wanted || children_min > p_budget)) {
			result.selected_nodes.write[selected_count++] = p_id;
			return node.payload_count;
		}
		uint64_t used = 0, remaining_min = children_min;
		for (uint32_t c = node.first_child; c < node.first_child + node.child_count; ++c) {
			remaining_min -= minimum[c];
			used += select(c, p_budget - used - remaining_min);
		}
		return used;
	};
	// Every possible cut is bounded by the admitted total payload. This also keeps
	// the unavailable sentinel above a caller-supplied UINT64_MAX budget.
	result.selected_splats = select(0, MIN(p_request.splat_budget, uint64_t(MAX_INPUT_SPLATS)));
	result.selected_nodes.resize(selected_count);
	result.heuristic_available = true;
	result.heuristic_satisfied = true;
	result.canonical_leaves = true;
	LocalVector<Gaussian> output;
	LocalVector<Vector3> output_sh;
	if (!fallible_resize(output, result.selected_splats, "HLODCut payload") ||
	    !fallible_resize(output_sh, result.selected_splats * high, "HLODCut SH")) {
		return fail(Status::ALLOCATION_FAILED, "Materialization allocation failed");
	}
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	if (!gs_resize_or_fail(chunks, selected_count, "HLODCut chunks")) {
		return fail(Status::ALLOCATION_FAILED, "Chunk allocation failed");
	}
	Bounds world_bounds;
	world_bounds.add_node(tree, tree.nodes[0]);
	const AABB source_bounds = p_source->get_bounds();
	if (!finite3(source_bounds.position) || !finite3(source_bounds.size) || source_bounds.size.x < 0 ||
	    source_bounds.size.y < 0 || source_bounds.size.z < 0) {
		return fail(Status::INVALID_INPUT, "Invalid source bounds");
	}
	for (int a = 0; a < 3; ++a) {
		world_bounds.add(a, source_bounds.position[a], double(source_bounds.position[a]) + source_bounds.size[a]);
	}
	uint32_t cursor = 0;
	for (uint32_t k = 0; k < selected_count; ++k) {
		const uint32_t id = result.selected_nodes[k];
		const auto &node = tree.nodes[id];
		bool known;
		result.heuristic_satisfied &= heuristic(id, known);
		result.heuristic_available &= known;
		result.canonical_leaves &= node.is_leaf();
		Bounds chunk_bounds;
		chunk_bounds.add_node(tree, node);
		double center[3];
		tree.node_cell_center(node, center);
		auto &chunk = chunks.write[k];
		if (!gs_resize_or_fail(chunk.indices, node.payload_count, "HLODCut indices")) {
			return fail(Status::ALLOCATION_FAILED, "Chunk index allocation failed");
		}
		const Gaussian *source = payload(node);
		const Vector3 *source_sh = sh_payload(node);
		for (uint32_t i = 0; i < node.payload_count; ++i) {
			Gaussian &g = output[cursor];
			g = source[i];
			if (!node.is_leaf()) {
				for (int a = 0; a < 3; ++a) {
					const double absolute = center[a] + double(source[i].position[a]);
					if (!std::isfinite(absolute) || std::abs(absolute) > std::numeric_limits<real_t>::max()) {
						return fail(Status::INVALID_INPUT, "Interior position conversion out of range");
					}
					g.position[a] = real_t(absolute);
					result.position_rounding_occurred |= double(g.position[a]) != absolute;
				}
			}
			for (uint32_t s = 0; s < high; ++s) {
				output_sh[uint64_t(cursor) * high + s] = source_sh[uint64_t(i) * high + s];
			}
			double covariance[6];
			splat_covariance(g, covariance);
			const int diagonal[] = {0, 3, 5};
			for (int a = 0; a < 3; ++a) {
				const double support = 3.0 * std::sqrt(MAX(covariance[diagonal[a]], 0.0));
				chunk_bounds.add(a, double(g.position[a]) - support, double(g.position[a]) + support);
			}
			chunk.indices.write[i] = cursor++;
		}
		if (!chunk_bounds.finish(chunk.bounds)) {
			return fail(Status::INVALID_INPUT, "Unrepresentable output chunk bounds");
		}
		chunk.center = chunk.bounds.get_center();
		double radius_squared = 0;
		for (int a = 0; a < 3; ++a) {
			const double lo = chunk.bounds.position[a];
			const double hi =
			        MAX(lo + double(chunk.bounds.size[a]), double(chunk.bounds.position[a] + chunk.bounds.size[a]));
			const double extent = MAX(std::abs(lo - chunk.center[a]), std::abs(hi - chunk.center[a]));
			radius_squared += extent * extent;
			world_bounds.add(a, lo, hi);
		}
		const double radius = std::sqrt(radius_squared);
		chunk.radius = float(radius);
		if (double(chunk.radius) < radius) {
			chunk.radius = std::nextafter(chunk.radius, float(INFINITY));
		}
		if (!Math::is_finite(chunk.radius)) {
			return fail(Status::INVALID_INPUT, "Unrepresentable output sphere");
		}
	}
	AABB bounds;
	if (!world_bounds.finish(bounds)) {
		return fail(Status::INVALID_INPUT, "Unrepresentable output world bounds");
	}
	if (!bake_allocation_probe(result.selected_splats * sizeof(Gaussian), "HLODCut world payload") ||
	    !bake_allocation_probe(result.selected_splats * high * sizeof(Vector3), "HLODCut world SH")) {
		return fail(Status::ALLOCATION_FAILED, "World payload allocation probe failed");
	}
	Ref<GaussianData> copied;
	copied.instantiate();
	copied->set_gaussian_payload(output, output_sh, snapshot.get_sh_first_order_count(), high, snapshot.get_2d_mode(),
	                             snapshot.get_antialiased());
	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_gaussian_data(copied);
	world->set_static_chunks(chunks);
	world->set_bounds(bounds);
	if (data->get_content_revision() != p_request.source_revision || !p_source->is_hlod_payload_current()) {
		return fail(Status::STALE, "Source changed during materialization");
	}
	result.world = world;
	result.status = Status::COMPLETE;
	result.coverage_complete = true;
	result.budget_satisfied = true;
	result.source_revision = p_request.source_revision;
	result.admitted_allocation_bytes = admitted;
	result.cpu_usec = OS::get_singleton()->get_ticks_usec() - started;
	return result;
}

} // namespace gs_hlod::runtime_cut
#endif
