#include "hlod_runtime_selection.h"

#include "../core/gs_vector_alloc.h"
#include <algorithm>
#include <cmath>

namespace gs_hlod_runtime {

bool CutHistory::reserve(uint32_t p_nodes) {
	if (p_nodes == 0 || p_nodes > GaussianSplatHlodRuntimeData::MAX_NODES) {
		return false;
	}
	// Fresh candidate vectors avoid COW detachment hidden behind later ptrw calls.
	Vector<Candidate> new_heap;
	Vector<uint32_t> new_selected, new_positions;
	Vector<uint64_t> new_refined;
	if (!gs_resize_or_fail(new_heap, p_nodes, "HLOD cut heap") ||
			!gs_resize_or_fail(new_selected, p_nodes, "HLOD cut selection") ||
			!gs_resize_or_fail(new_positions, p_nodes, "HLOD cut positions") ||
			!gs_resize_or_fail(new_refined, p_nodes, "HLOD cut history")) {
		return false;
	}
	new_refined.fill(0);
	heap = std::move(new_heap);
	selected = std::move(new_selected);
	selected_position = std::move(new_positions);
	refined = std::move(new_refined);
	capacity = p_nodes;
	epoch = 0;
	return true;
}

void CutHistory::invalidate() {
	refined.fill(0);
	epoch = 0;
}

uint64_t CutHistory::allocated_bytes() const {
	uint64_t bytes = 0, one = 0;
	allocation_bytes(capacity, sizeof(Candidate), one); bytes += one;
	allocation_bytes(capacity, sizeof(uint32_t), one); bytes += one * 2;
	allocation_bytes(capacity, sizeof(uint64_t), one); bytes += one;
	return bytes;
}

CutHistory::Result CutHistory::select(const GaussianSplatHlodRuntimeData &p_data, const CutRequest &p_request) {
	Result out;
	const uint32_t count = p_data.nodes.size();
	if (count == 0 || count > capacity || p_request.node_budget == 0 || p_request.node_budget > capacity ||
			!p_request.camera.is_finite() || !std::isfinite(p_request.focal_pixels) || p_request.focal_pixels <= 0 ||
			!std::isfinite(p_request.tau_pixels) || p_request.tau_pixels <= 0 ||
			!std::isfinite(p_request.record_budget) || p_request.record_budget < 0 ||
			!std::isfinite(p_request.tile_area) || p_request.tile_area <= 0 ||
			(p_request.resident && p_request.resident_count != count)) {
		return out;
	}
	if (epoch == UINT64_MAX) {
		invalidate();
	}
	const uint64_t previous = epoch++;
	uint32_t heap_count = 0;
	auto available = [&](uint32_t p_node) { return !p_request.resident || p_request.resident[p_node] != 0; };
	auto visible = [&](uint32_t p_node) {
		const auto &n = p_data.nodes[p_node];
		if (!p_request.frustum_enabled) { return true; }
		for (const Plane &plane : p_request.frustum) {
			if (plane.distance_to(n.bounds_center) > n.topology.radius) { return false; }
		}
		return true;
	};
	auto distance = [&](uint32_t p_node) {
		const auto &n = p_data.nodes[p_node];
		const double dx = double(n.bounds_center.x) - p_request.camera.x;
		const double dy = double(n.bounds_center.y) - p_request.camera.y;
		const double dz = double(n.bounds_center.z) - p_request.camera.z;
		return MAX(std::sqrt(dx * dx + dy * dy + dz * dz) - double(n.topology.radius), 1.0e-6);
	};
	auto error = [&](uint32_t p_node) {
		return double(p_data.nodes[p_node].topology.geometric_error) * p_request.focal_pixels / distance(p_node);
	};
	auto cost = [&](uint32_t p_node) {
		const auto &n = p_data.nodes[p_node];
		// The overlap estimate is a per-node model, not a per-splat bound: the node's splats are assumed no nearer than
		// the near surface of its bounding sphere. A camera inside that sphere makes the near-surface distance 0 and the
		// estimate unbounded (1e17 records at 2 m inside the holzbank, where the measured demand is 5.5e5), so no frontier
		// could ever be admitted. Floor it at a fraction of the radius: a splat set spread over a surface of the sphere
		// has most of its splats at least that far from a camera inside it. Outside the sphere the floor is inactive
		// beyond radius * (1 + fraction), so the estimate stays the sound near-surface bound there.
		const double d = MAX(distance(p_node), kCostInsideDistanceFraction * double(n.topology.radius));
		return double(n.topology.payload_count) + n.footprint * p_request.focal_pixels * p_request.focal_pixels /
				(d * d * p_request.tile_area);
	};
	auto lower_priority = [](const Candidate &a, const Candidate &b) {
		return a.priority == b.priority ? a.node > b.node : a.priority < b.priority;
	};
	auto enqueue = [&](uint32_t p_node) {
		if (p_data.nodes[p_node].topology.child_count == 0) { return; }
		const double e = error(p_node);
		const bool was_refined = previous != 0 && refined[p_node] == previous;
		if (!(e > 1.25 * p_request.tau_pixels || (was_refined && e > p_request.tau_pixels))) { return; }
		heap.write[heap_count++] = { p_node, e * (was_refined ? 1.25 : 1.0) };
		std::push_heap(heap.ptrw(), heap.ptrw() + heap_count, lower_priority);
	};
	out.visited_nodes = 1;
	if (!visible(0)) { out.status = EMPTY; return out; }
	if (!available(0)) { out.status = UNAVAILABLE; return out; }
	const double root_cost = cost(0);
	if (!std::isfinite(root_cost)) { return out; }
	const double record_cap = 0.8 * p_request.record_budget; // The 80 percent admission factor.
	selected.write[0] = 0;
	selected_position.write[0] = 0;
	out.selected_count = 1;
	out.selected_splats = p_data.nodes[0].topology.payload_count;
	out.estimated_records = root_cost;
	if (out.selected_splats > p_request.splat_budget || root_cost > record_cap) {
		// Bootstrap (ADR 14.2): expand the costliest expandable frontier node until the frontier fits
		// every cap. The order depends only on tree, camera and residency, never on budgets, so the
		// expansion sequence is a fixed prefix order and raising a budget only stops it earlier.
		// Work is bounded by a visit limit and uses the preallocated heap/selection storage.
		out.budget_bound = true;
		out.bootstrapped = true;
		const uint64_t work_limit = 1 + uint64_t(p_request.node_budget) * gs_hlod::kMaxChildren;
		bool missing_residency = false;
		auto fail = [&](Status p_status) {
			out.status = p_status;
			out.selected_count = 0;
			out.selected_splats = 0;
			out.estimated_records = 0.0;
			return out;
		};
		auto fits = [&]() {
			return out.selected_splats <= p_request.splat_budget && out.selected_count <= p_request.node_budget &&
					out.estimated_records <= record_cap;
		};
		auto push_expandable = [&](uint32_t p_node, double p_cost) {
			if (p_data.nodes[p_node].topology.child_count == 0) { return; }
			heap.write[heap_count++] = { p_node, p_cost };
			std::push_heap(heap.ptrw(), heap.ptrw() + heap_count, lower_priority);
		};
		push_expandable(0, root_cost);
		while (!fits()) {
			if (heap_count == 0) {
				return fail(missing_residency ? BOOTSTRAP_MISSING_RESIDENCY : BOOTSTRAP_INFEASIBLE);
			}
			std::pop_heap(heap.ptrw(), heap.ptrw() + heap_count, lower_priority);
			const uint32_t parent = heap[--heap_count].node;
			const auto &n = p_data.nodes[parent].topology;
			if (n.child_count > gs_hlod::kMaxChildren || n.first_child > count || n.child_count > count - n.first_child) {
				out.status = INVALID; out.selected_count = 0; out.selected_splats = 0; out.estimated_records = 0.0; return out;
			}
			uint32_t children[gs_hlod::kMaxChildren];
			double child_costs[gs_hlod::kMaxChildren];
			uint32_t visible_children = 0;
			uint64_t child_splats = 0;
			double child_cost = 0.0;
			bool children_ready = true;
			for (uint32_t j = 0; j < n.child_count; ++j) {
				const uint32_t child = n.first_child + j;
				if (++out.visited_nodes > work_limit) { return fail(BOOTSTRAP_EXHAUSTED); }
				if (!visible(child)) { continue; }
				if (!available(child)) { children_ready = false; break; }
				const double c = cost(child);
				if (!std::isfinite(c)) { return fail(BOOTSTRAP_ARITHMETIC); }
				children[visible_children] = child;
				child_costs[visible_children++] = c;
				child_splats += p_data.nodes[child].topology.payload_count;
				child_cost += c;
			}
			if (!children_ready) { missing_residency = true; continue; } // Parent stays; no partial replacement.
			const uint32_t next_count = out.selected_count - 1 + visible_children;
			if (next_count > p_request.node_budget) { continue; } // Node cap refuses this expansion only.
			const double next_cost = MAX(0.0, out.estimated_records - cost(parent)) + child_cost;
			if (!std::isfinite(next_cost)) { return fail(BOOTSTRAP_ARITHMETIC); }
			const uint32_t removed = selected_position[parent];
			const uint32_t last = selected[--out.selected_count];
			selected.write[removed] = last;
			selected_position.write[last] = removed;
			for (uint32_t j = 0; j < visible_children; ++j) {
				selected.write[out.selected_count] = children[j];
				selected_position.write[children[j]] = out.selected_count++;
				push_expandable(children[j], child_costs[j]);
			}
			out.selected_splats = out.selected_splats - n.payload_count + child_splats;
			out.estimated_records = next_cost;
			refined.write[parent] = epoch; // Same hysteresis stamp as ordinary refinement.
		}
		// Refinement below starts from the complete frontier, not from the bootstrap heap.
		heap_count = 0;
		for (uint32_t i = 0; i < out.selected_count; ++i) { enqueue(selected[i]); }
	} else {
		enqueue(0);
	}
	while (heap_count != 0) {
		std::pop_heap(heap.ptrw(), heap.ptrw() + heap_count, lower_priority);
		const uint32_t parent = heap[--heap_count].node;
		const auto &n = p_data.nodes[parent].topology;
		uint32_t children[gs_hlod::kMaxChildren];
		uint32_t visible_children = 0;
		uint64_t child_splats = 0;
		double child_cost = 0;
		bool complete = true;
		if (n.child_count > gs_hlod::kMaxChildren || n.first_child > count || n.child_count > count - n.first_child) {
			out.status = INVALID; return out;
		}
		for (uint32_t j = 0; j < n.child_count; ++j) {
			const uint32_t child = n.first_child + j;
			++out.visited_nodes;
			if (!visible(child)) { continue; }
			if (!available(child)) { complete = false; break; }
			children[visible_children++] = child;
			child_splats += p_data.nodes[child].topology.payload_count;
			child_cost += cost(child);
		}
		if (!complete) { continue; }
		const uint64_t next_splats = out.selected_splats - n.payload_count + child_splats;
		const uint32_t next_count = out.selected_count - 1 + visible_children;
		const double next_cost = MAX(0.0, out.estimated_records - cost(parent)) + child_cost;
		if (!std::isfinite(next_cost)) { out.status = INVALID; return out; }
		if (next_splats > p_request.splat_budget || next_count > p_request.node_budget || next_cost > record_cap) {
			out.budget_bound = true;
			break; // ADR: first cap refusal stops the whole priority walk.
		}
		const uint32_t removed = selected_position[parent];
		const uint32_t last = selected[--out.selected_count];
		selected.write[removed] = last;
		selected_position.write[last] = removed;
		for (uint32_t j = 0; j < visible_children; ++j) {
			selected.write[out.selected_count] = children[j];
			selected_position.write[children[j]] = out.selected_count++;
			enqueue(children[j]);
		}
		refined.write[parent] = epoch;
		out.selected_splats = next_splats;
		out.estimated_records = next_cost;
	}
	// Stable publication order, independent of heap storage and swap-removal order.
	std::sort(selected.ptrw(), selected.ptrw() + out.selected_count);
	out.status = out.selected_count ? COMPLETE : EMPTY;
	return out;
}
}
