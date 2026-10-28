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
	Vector<uint32_t> new_demand;
	if (!gs_resize_or_fail(new_demand, p_nodes, "HLOD cut demand") ||
			!gs_resize_or_fail(new_heap, p_nodes, "HLOD cut heap") ||
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
	demand = std::move(new_demand);
	demand_size = 0;
	capacity = p_nodes;
	epoch = 0;
	return true;
}

void CutHistory::invalidate() {
	demand_size = 0;
	refined.fill(0);
	epoch = 0;
}

uint64_t CutHistory::workspace_bytes(uint32_t p_nodes) {
	uint64_t bytes = 0, one = 0;
	if (!allocation_bytes(p_nodes, sizeof(Candidate), one)) { return 0; } bytes += one;
	if (!allocation_bytes(p_nodes, sizeof(uint32_t), one)) { return 0; } bytes += one * 3; // selection, positions and demand
	if (!allocation_bytes(p_nodes, sizeof(uint64_t), one)) { return 0; } bytes += one;
	return bytes;
}

uint64_t CutHistory::allocated_bytes() const {
	return workspace_bytes(capacity);
}

CutHistory::Result CutHistory::select(const GaussianSplatHlodRuntimeData &p_data, const CutRequest &p_request) {
	Result out;
	const uint32_t count = p_data.nodes.size();
	if (count == 0 || count > capacity || p_request.node_budget == 0 || p_request.node_budget > capacity ||
			!p_request.camera.is_finite() || !std::isfinite(p_request.focal_pixels) || p_request.focal_pixels <= 0 ||
			!std::isfinite(p_request.tau_pixels) || p_request.tau_pixels <= 0 ||
			!std::isfinite(p_request.record_budget) || p_request.record_budget < 0 ||
			!std::isfinite(p_request.tile_area) || p_request.tile_area <= 0 ||
			(p_request.resident && p_request.resident_count != count) ||
			(p_request.tie_rank && p_request.tie_rank_count != count)) {
		return out;
	}
	if (epoch == UINT64_MAX) {
		invalidate();
	}
	demand_size = 0;
	partial_size = 0;
	// A node can be listed more than once (a bootstrap expansion demands the children of a parent the refinement
	// walk may demand again), so the list is capped at the node count and consumers (NodeResidency::request)
	// treat duplicates as one demand.
	// A group is demanded whole or not at all: a truncated group would request part of a transition that can
	// then never complete.
	auto push_group = [&](const uint32_t *p_nodes, uint32_t p_count) {
		if (demand_size + p_count > capacity) { return; }
		for (uint32_t i = 0; i < p_count; ++i) { demand.write[demand_size++] = p_nodes[i]; }
	};
	auto push_demand = [&](uint32_t p_node) { push_group(&p_node, 1); };
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
		// Admission heuristic with no guaranteed upper bound (ADR 6.5; overflow_splats_clamped stays the tripwire and the
		// overlap storage grows to 1.5x the measured demand). The footprint term uses the distance to the near surface of
		// the node's bounding sphere. That distance is 0 for a camera inside the sphere, which makes the estimate unbounded
		// (1e17 records at 2 m inside the holzbank, where 5.5e5 were measured) and no frontier admissible. So the distance
		// is floored at a fraction of the radius, modelling the splats as spread over a surface of the sphere. The floor
		// is active wherever the centre distance is below radius * (1 + fraction): inside the sphere AND in the shell just
		// outside it, where it lowers the estimate relative to the plain near-surface distance. It is continuous in the
		// camera position. Beyond that shell the estimate is unchanged.
		const double d = MAX(distance(p_node), kCostInsideDistanceFraction * double(n.topology.radius));
		return double(n.topology.payload_count) + n.footprint * p_request.focal_pixels * p_request.focal_pixels /
				(d * d * p_request.tile_area);
	};
	auto lower_priority = [](const Candidate &a, const Candidate &b) {
		if (a.priority != b.priority) { return a.priority < b.priority; }
		return a.tie == b.tie ? a.node > b.node : a.tie > b.tie;
	};
	auto tie_of = [&](uint32_t p_node) { return p_request.tie_rank ? p_request.tie_rank[p_node] : p_node; };
	auto enqueue = [&](uint32_t p_node) {
		if (p_data.nodes[p_node].topology.child_count == 0) { return; }
		const double e = error(p_node);
		const bool was_refined = previous != 0 && refined[p_node] == previous;
		if (!(e > 1.25 * p_request.tau_pixels || (was_refined && e > p_request.tau_pixels))) { return; }
		heap.write[heap_count++] = { p_node, tie_of(p_node), e * (was_refined ? 1.25 : 1.0) };
		std::push_heap(heap.ptrw(), heap.ptrw() + heap_count, lower_priority);
	};
	auto pages_of = [&](uint32_t p_node) -> uint64_t {
		if (p_request.page_splats == 0) { return 0; }
		return (uint64_t(p_data.nodes[p_node].topology.payload_count) + p_request.page_splats - 1u) / p_request.page_splats;
	};
	// Pages that stay resident: the cut and all of its ancestors. Refining a node keeps it (as an ancestor) and
	// adds its children, so the closure only grows by the children's pages.
	uint64_t closure_pages = pages_of(0);
	out.visited_nodes = 1;
	if (!visible(0)) { out.status = EMPTY; return out; }
	if (!available(0)) { push_demand(0); out.status = UNAVAILABLE; return out; }
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
			// `selected` still holds the frontier reached so far (all resident); only the count is cleared.
			partial_size = p_status == BOOTSTRAP_MISSING_RESIDENCY ? out.selected_count : 0u;
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
			heap.write[heap_count++] = { p_node, tie_of(p_node), p_cost };
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
			uint64_t child_pages = 0;
			uint32_t visible_children = 0;
			uint64_t child_splats = 0;
			double child_cost = 0.0;
			bool children_ready = true;
			uint32_t missing_children[gs_hlod::kMaxChildren];
			uint32_t missing_count = 0;
			for (uint32_t j = 0; j < n.child_count; ++j) {
				const uint32_t child = n.first_child + j;
				if (++out.visited_nodes > work_limit) { return fail(BOOTSTRAP_EXHAUSTED); }
				if (!visible(child)) { continue; }
				if (!available(child)) { children_ready = false; missing_children[missing_count++] = child; continue; }
				const double c = cost(child);
				if (!std::isfinite(c)) { return fail(BOOTSTRAP_ARITHMETIC); }
				child_pages += pages_of(child);
				children[visible_children] = child;
				child_costs[visible_children++] = c;
				child_splats += p_data.nodes[child].topology.payload_count;
				child_cost += c;
			}
			if (!children_ready) {
				// Parent stays; no partial replacement. The bootstrap needs this expansion, so every missing
				// visible child is demanded together (a whole transition, never a subset).
				missing_residency = true;
				push_group(missing_children, missing_count);
				continue;
			}
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
			closure_pages += child_pages;
			refined.write[parent] = epoch; // Same hysteresis stamp as ordinary refinement.
		}
		// Refinement below starts from the complete frontier, not from the bootstrap heap.
		heap_count = 0;
		for (uint32_t i = 0; i < out.selected_count; ++i) { enqueue(selected[i]); }
	} else {
		enqueue(0);
	}
	// Plan = the actual selection plus the transitions demanded so far (as if their children were resident).
	// The cut itself never includes a virtual transition; the plan only decides what is worth asking for, so a
	// lower-priority group is not requested when the higher-priority one already uses the budget it needs.
	int64_t plan_splats = 0, plan_count = 0;
	double plan_cost = 0.0;
	uint64_t plan_pages = 0;
	while (heap_count != 0) {
		std::pop_heap(heap.ptrw(), heap.ptrw() + heap_count, lower_priority);
		const uint32_t parent = heap[--heap_count].node;
		const auto &n = p_data.nodes[parent].topology;
		uint32_t children[gs_hlod::kMaxChildren];
		uint32_t visible_children = 0;
		uint64_t child_splats = 0;
		double child_cost = 0;
		bool complete = true;
		uint32_t missing_children[gs_hlod::kMaxChildren];
		uint32_t missing_count = 0;
		uint64_t child_pages = 0;
		if (n.child_count > gs_hlod::kMaxChildren || n.first_child > count || n.child_count > count - n.first_child) {
			out.status = INVALID; return out;
		}
		for (uint32_t j = 0; j < n.child_count; ++j) {
			const uint32_t child = n.first_child + j;
			++out.visited_nodes;
			if (!visible(child)) { continue; }
			if (!available(child)) { complete = false; missing_children[missing_count++] = child; }
			children[visible_children++] = child;
			child_pages += pages_of(child);
			child_splats += p_data.nodes[child].topology.payload_count;
			child_cost += cost(child);
		}
		const uint64_t next_splats = out.selected_splats - n.payload_count + child_splats;
		const uint32_t next_count = out.selected_count - 1 + visible_children;
		const double next_cost = MAX(0.0, out.estimated_records - cost(parent)) + child_cost;
		if (!std::isfinite(next_cost)) { out.status = INVALID; return out; }
		if (!complete) {
			// Residency miss: the parent stays and the walk continues (unchanged). The whole transition is
			// demanded only when it would fit every cap together with the transitions already demanded, so no
			// bandwidth goes to children whose replacement the budget would refuse once the higher-priority
			// ones are resident (the walk stops at the first refusal).
			const int64_t d_splats = int64_t(child_splats) - int64_t(n.payload_count);
			const int64_t d_count = int64_t(visible_children) - 1;
			const double d_cost = child_cost - cost(parent);
			if (int64_t(out.selected_splats) + plan_splats + d_splats <= int64_t(p_request.splat_budget) &&
					int64_t(out.selected_count) + plan_count + d_count <= int64_t(p_request.node_budget) &&
					MAX(0.0, out.estimated_records + plan_cost + d_cost) <= record_cap &&
					(p_request.page_splats == 0 || closure_pages + plan_pages + child_pages <= p_request.page_budget)) {
				push_group(missing_children, missing_count);
				plan_pages += child_pages;
				plan_splats += d_splats;
				plan_count += d_count;
				plan_cost += d_cost;
			}
			continue;
		}
		if (next_splats > p_request.splat_budget || next_count > p_request.node_budget || next_cost > record_cap) {
			out.budget_bound = true;
			break; // ADR: first cap refusal stops the whole priority walk.
		}
		closure_pages += child_pages;
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
