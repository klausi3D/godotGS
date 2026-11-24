#include "hlod_runtime_selection.h"

#include "../core/gs_vector_alloc.h"
#include <algorithm>
#include <cmath>
#include <limits>

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
	Vector<TransitionGroup> new_groups;
	Vector<uint8_t> new_diagnostics;
	Vector<float> new_fade, new_shown;
	Vector<uint32_t> new_stamp, new_mark, new_prev;
	Vector<FadeGroup> new_fade_groups, new_next_groups;
	Vector<FadeMember> new_members, new_next_members;
	const uint32_t group_cap = MIN(p_nodes, kMaxFadeGroups), member_cap = MIN(p_nodes, kMaxFadeMembers);
	if (!gs_resize_or_fail(new_fade, p_nodes, "HLOD cut fade factors") ||
			!gs_resize_or_fail(new_shown, p_nodes, "HLOD cut drawn weights") ||
			!gs_resize_or_fail(new_stamp, p_nodes, "HLOD cut drawn stamps") ||
			!gs_resize_or_fail(new_mark, p_nodes, "HLOD cut fade marks") ||
			!gs_resize_or_fail(new_prev, p_nodes, "HLOD cut previous drawn set") ||
			!gs_resize_or_fail(new_fade_groups, group_cap, "HLOD cut fade groups") ||
			!gs_resize_or_fail(new_next_groups, group_cap, "HLOD cut fade groups") ||
			!gs_resize_or_fail(new_members, member_cap, "HLOD cut fade members") ||
			!gs_resize_or_fail(new_next_members, member_cap, "HLOD cut fade members") ||
			!gs_resize_or_fail(new_demand, p_nodes, "HLOD cut demand") ||
			!gs_resize_or_fail(new_groups, p_nodes, "HLOD cut transition groups") ||
			!gs_resize_or_fail(new_diagnostics, p_nodes, "HLOD cut diagnostics") ||
			!gs_resize_or_fail(new_heap, p_nodes, "HLOD cut heap") ||
			!gs_resize_or_fail(new_selected, p_nodes, "HLOD cut selection") ||
			!gs_resize_or_fail(new_positions, p_nodes, "HLOD cut positions") ||
			!gs_resize_or_fail(new_refined, p_nodes, "HLOD cut history")) {
		return false;
	}
	new_refined.fill(0);
	new_diagnostics.fill(0);
	heap = std::move(new_heap);
	selected = std::move(new_selected);
	selected_position = std::move(new_positions);
	refined = std::move(new_refined);
	demand = std::move(new_demand);
	demand_groups = std::move(new_groups);
	diagnostics = std::move(new_diagnostics);
	fade = std::move(new_fade);
	new_stamp.fill(0);
	new_mark.fill(0);
	shown = std::move(new_shown);
	drawn_stamp = std::move(new_stamp);
	group_mark = std::move(new_mark);
	prev_drawn = std::move(new_prev);
	groups = std::move(new_fade_groups);
	next_groups = std::move(new_next_groups);
	members = std::move(new_members);
	next_members = std::move(new_next_members);
	prev_drawn_count = group_count = member_count = 0;
	demand_size = 0;
	capacity = p_nodes;
	epoch = 0;
	return true;
}

void CutHistory::invalidate() {
	demand_size = 0;
	refined.fill(0);
	diagnostics.fill(0);
	drawn_stamp.fill(0);
	group_mark.fill(0);
	prev_drawn_count = group_count = member_count = 0;
	epoch = 0;
}

void CutHistory::invalidate_refinement() {
	// A stamp of 0 never equals the previous epoch of a selection (>= 1 once any selection ran), so no node counts as
	// refined last time. drawn_stamp, shown, prev_drawn and the fade groups are untouched.
	demand_size = 0;
	refined.fill(0);
	diagnostics.fill(0);
}

uint64_t CutHistory::workspace_bytes(uint32_t p_nodes) {
	uint64_t bytes = 0, one = 0;
	if (!allocation_bytes(p_nodes, sizeof(Candidate), one)) { return 0; } bytes += one;
	if (!allocation_bytes(p_nodes, sizeof(uint32_t), one)) { return 0; } bytes += one * 3; // selection, positions and demand
	if (!allocation_bytes(p_nodes, sizeof(uint64_t), one)) { return 0; } bytes += one;
	if (!allocation_bytes(p_nodes, sizeof(TransitionGroup), one)) { return 0; } bytes += one;
	if (!allocation_bytes(p_nodes, sizeof(float), one)) { return 0; } bytes += one * 2; // Fade factors, drawn weights (S6).
	if (!allocation_bytes(p_nodes, sizeof(uint32_t), one)) { return 0; } bytes += one * 3; // Drawn stamps, marks, previous set.
	if (!allocation_bytes(MIN(p_nodes, kMaxFadeGroups), sizeof(FadeGroup), one)) { return 0; } bytes += one * 2;
	if (!allocation_bytes(MIN(p_nodes, kMaxFadeMembers), sizeof(FadeMember), one)) { return 0; } bytes += one * 2;
	return bytes;
}

uint64_t CutHistory::allocated_bytes() const {
	uint64_t diagnostic_bytes = 0;
	allocation_bytes(capacity, sizeof(uint8_t), diagnostic_bytes);
	return workspace_bytes(capacity) + diagnostic_bytes;
}

CutHistory::Result CutHistory::select(const GaussianSplatHlodRuntimeData &p_data, const CutRequest &p_request) {
	if (p_request.fade_frames == 0) { return select_cut(p_data, p_request); }
	if (epoch >= 0x7fffffffu) { invalidate(); } // Drawn stamps keep 31 epoch bits.
	const uint64_t previous = epoch;
	// The cut and its band fades are selected against the full caps, exactly as without the temporal fade: a fade never
	// coarsens the cut (a fixed reserve did, and a budget-bound wall then let an object behind it show through). The old
	// configurations of fading regions are drawn only from the headroom the cut leaves.
	Result out = select_cut(p_data, p_request);
	if (out.status == COMPLETE || out.status == EMPTY) { apply_temporal_fade(p_data, p_request, previous, out); }
	return out;
}

CutHistory::Result CutHistory::select_cut(const GaussianSplatHlodRuntimeData &p_data, const CutRequest &p_request) {
	Result out;
	diagnostics.fill(0);
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
	auto push_group = [&](const uint32_t *p_nodes, uint32_t p_count, TransitionGroup p_group) {
		if (demand_size + p_count > capacity) { return false; }
		p_group.ordinal = demand_size;
		for (uint32_t i = 0; i < p_count; ++i) {
			demand_groups.write[demand_size] = p_group;
			demand.write[demand_size++] = p_nodes[i];
		}
		return true;
	};
	auto push_demand = [&](uint32_t p_node) { push_group(&p_node, 1, { UINT32_MAX, p_node, 0, 1, TRANSITION_ROOT }); };
	auto refused = [&](uint32_t p_node) {
		if (out.first_cap_refused_node < 0) { out.first_cap_refused_node = int32_t(p_node); }
	};
	auto missed = [&](uint32_t p_node, bool p_demanded) {
		if (!(diagnostics[p_node] & RESIDENCY_MISSED)) { ++out.residency_missed_count; }
		diagnostics.write[p_node] |= RESIDENCY_MISSED | (p_demanded ? MISS_DEMANDED : 0);
	};
	const uint64_t previous = epoch++;
	uint32_t heap_count = 0;
	auto available = [&](uint32_t p_node) { return !p_request.resident || p_request.resident[p_node] != 0; };
	// Package F1 (ADR 14.5): an ancestor of an edited leaf is STALE. It is never selected, never a fallback and never
	// demanded; the walk replaces it by its children whatever the quality rule or the caps say. A stale node is
	// "available" only in the sense that it needs no residency: it is never drawn, only expanded.
	auto stale = [&](uint32_t p_node) { return p_data.is_stale(p_node); };
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
		// With the cross-fade the band is a blend, not hysteresis: every node above tau is a candidate.
		const bool wanted = p_request.crossfade ? e > p_request.tau_pixels :
				(e > kRefineBandFactor * p_request.tau_pixels || (was_refined && e > p_request.tau_pixels));
		if (!wanted) { return; }
		heap.write[heap_count++] = { p_node, tie_of(p_node), e * (was_refined ? kRefineBandFactor : 1.0) };
		std::push_heap(heap.ptrw(), heap.ptrw() + heap_count, lower_priority);
	};
	auto pages_of = [&](uint32_t p_node) -> uint64_t {
		if (p_request.page_splats == 0 || stale(p_node)) { return 0; } // A stale node holds no pages once the edit is committed.
		return (uint64_t(p_data.nodes[p_node].topology.payload_count) + p_request.page_splats - 1u) / p_request.page_splats;
	};
	out.closure_measured = p_request.page_splats != 0;
	if (out.closure_measured) {
		out.placement_reserve_state = p_data.placement_reserve_prepared ? RESERVE_MEASURED : RESERVE_UNPREPARED;
	}
	out.placement_reserve_measured = out.placement_reserve_state == RESERVE_MEASURED;
	if (out.placement_reserve_measured) {
		out.placement_reserve_pages = (uint64_t(p_data.max_node_payload_count) + p_request.page_splats - 1u) / p_request.page_splats;
		// When every live node fits, no eviction/placement headroom is needed. Atlas sizing already caps
		// small worlds at this exact total; reserving more would permanently prevent their full refinement.
		// A different page geometry keeps the conservative reserve rather than trusting an incompatible sum.
		if (p_data.placement_page_splats == p_request.page_splats && p_data.placement_node_pages <= p_request.page_budget) {
			out.placement_reserve_pages = 0;
		}
	}
	out.page_budget = p_request.page_budget;
	const uint64_t admission_pages = p_request.page_budget > out.placement_reserve_pages ?
			p_request.page_budget - out.placement_reserve_pages : 0;
	// Observational union, independent of closure_pages/plan_pages used by admission.
	// Every visited refinement already has its parent in this union. Unique marks also cover
	// bootstrap demand encountered again by the ordinary walk without double charging it.
	auto report_pages = [&](uint32_t p_node, uint8_t p_bypass) {
		const uint64_t pages = pages_of(p_node);
		uint8_t &flags = diagnostics.write[p_node];
		if (!(flags & PAGE_ACCOUNTED)) { out.demanded_closure_pages += pages; flags |= PAGE_ACCOUNTED; }
		if (p_bypass == BOOTSTRAP_PAGE_ACCOUNTED && !(flags & BOOTSTRAP_PAGE_ACCOUNTED)) {
			out.bootstrap_bypass_pages += pages; flags |= BOOTSTRAP_PAGE_ACCOUNTED;
		}
		if (p_bypass == RESIDENT_PAGE_ACCOUNTED && !(flags & RESIDENT_PAGE_ACCOUNTED)) {
			out.resident_refinement_bypass_pages += pages; flags |= RESIDENT_PAGE_ACCOUNTED;
		}
	};
	// Pages that stay resident: the cut and all of its ancestors. Refining a node keeps it (as an ancestor) and
	// adds its children, so the closure only grows by the children's pages.
	uint64_t closure_pages = pages_of(0);
	uint64_t plan_pages = 0;
	bool reserve_blocked = out.placement_reserve_measured && closure_pages > admission_pages;
	auto unplanned_pages = [&](uint32_t p_parent, uint64_t p_child_pages) -> uint64_t {
		return diagnostics[p_parent] & TRANSITION_PLANNED ? 0 : p_child_pages;
	};
	// One page predicate for bootstrap, resident refinement and demand. Reserve whole child groups,
	// retaining the parent/ancestors and all previously emitted transitions. Subtractions avoid overflow.
	auto transition_fits = [&](uint32_t p_parent, uint64_t p_child_pages) {
		if (p_request.page_splats == 0) { return true; }
		const uint64_t additional = unplanned_pages(p_parent, p_child_pages);
		const bool fits = closure_pages <= admission_pages && plan_pages <= admission_pages - closure_pages &&
				additional <= admission_pages - closure_pages - plan_pages;
		const bool physically_fits = closure_pages <= p_request.page_budget && plan_pages <= p_request.page_budget - closure_pages &&
				additional <= p_request.page_budget - closure_pages - plan_pages;
		reserve_blocked |= !fits && physically_fits;
		if (!fits) { ++out.page_refused_groups; refused(p_parent); }
		return fits;
	};
	auto reserve_transition = [&](uint32_t p_parent, uint64_t p_child_pages) {
		plan_pages += unplanned_pages(p_parent, p_child_pages);
		diagnostics.write[p_parent] |= TRANSITION_PLANNED;
	};
	out.visited_nodes = 1;
	if (!visible(0)) { out.status = EMPTY; return out; }
	report_pages(0, 0);
	// Every possible closure contains the root. Neither loading it nor refining it can fix this cap.
	if (p_request.page_splats != 0 && closure_pages > p_request.page_budget) {
		++out.page_refused_groups; refused(0); out.status = BOOTSTRAP_INFEASIBLE; return out;
	}
	const bool root_stale = stale(0);
	if (!root_stale && !available(0)) { push_demand(0); out.status = UNAVAILABLE; return out; }
	const double root_cost = cost(0);
	if (!std::isfinite(root_cost)) { return out; }
	const double record_cap = kRecordAdmissionFactor * p_request.record_budget; // The 80 percent admission factor.
	selected.write[0] = 0;
	selected_position.write[0] = 0;
	out.selected_count = 1;
	out.selected_splats = p_data.nodes[0].topology.payload_count;
	out.estimated_records = root_cost;
	if (out.selected_splats > p_request.splat_budget || root_cost > record_cap || root_stale) {
		// Bootstrap (ADR 14.2): expand the costliest expandable frontier node until the frontier fits
		// every cap. Candidate priority depends only on tree, camera and residency. Node/page caps may
		// refuse one expansion while another reaches a complete affordable frontier; no partial group is taken.
		// Work is bounded by a visit limit and uses the preallocated heap/selection storage.
		const bool caps_exceeded = out.selected_splats > p_request.splat_budget || root_cost > record_cap;
		out.budget_bound = caps_exceeded;
		out.bootstrapped = caps_exceeded;
		out.edit_forced = root_stale;
		uint32_t stale_selected = root_stale ? 1u : 0u;
		const uint64_t work_limit = 1 + uint64_t(p_request.node_budget) * gs_hlod::kMaxChildren;
		bool missing_residency = false;
		auto fail = [&](SelectStatus p_status) {
			// Only resident non-stale frontier nodes may receive keep ownership. A stale ancestor can
			// still be in this working frontier when its mandatory expansion is blocked on residency.
			partial_size = 0;
			if (p_status == BOOTSTRAP_MISSING_RESIDENCY) {
				for (uint32_t i = 0; i < out.selected_count; ++i) {
					const uint32_t node = selected[i];
					if (!stale(node) && available(node)) { selected.write[partial_size++] = node; }
				}
			}
			out.status = p_status;
			out.selected_count = 0;
			out.selected_splats = 0;
			out.estimated_records = 0.0;
			return out;
		};
		auto fits = [&]() {
			return stale_selected == 0 && out.selected_splats <= p_request.splat_budget && out.selected_count <= p_request.node_budget &&
					out.estimated_records <= record_cap;
		};
		auto push_expandable = [&](uint32_t p_node, double p_cost) {
			if (p_data.nodes[p_node].topology.child_count == 0) { return; }
			// A stale node outranks every quality-driven expansion: it can never stay in the cut.
			heap.write[heap_count++] = { p_node, tie_of(p_node), stale(p_node) ? std::numeric_limits<double>::infinity() : p_cost };
			std::push_heap(heap.ptrw(), heap.ptrw() + heap_count, lower_priority);
		};
		push_expandable(0, root_cost);
		while (!fits()) {
			// #1472: a frontier (the root included) whose splats and nodes fit and that holds no stale node is refused by
			// the record cap alone. The cheapest one is the demand: reported whatever the bootstrap finds or fails below.
			if (stale_selected == 0 && out.selected_splats <= p_request.splat_budget && out.selected_count <= p_request.node_budget) {
				out.record_demand = out.record_demand > 0.0 ? MIN(out.record_demand, out.estimated_records) : out.estimated_records;
			}
			if (heap_count == 0) {
				return fail(missing_residency ? BOOTSTRAP_MISSING_RESIDENCY : BOOTSTRAP_INFEASIBLE);
			}
			const bool caps_ok = out.selected_splats <= p_request.splat_budget && out.selected_count <= p_request.node_budget &&
					out.estimated_records <= record_cap;
			if (!caps_ok) {
				out.budget_bound = out.bootstrapped = true; // A forced stale expansion (F1) pushed the frontier over a cap.
			} else if (stale_selected != 0 && !stale(heap[0].node)) {
				// Only stale nodes are left to replace and every expandable stale node was tried (stale outranks all):
				// the rest are blocked on residency or on the node cap. Expanding unrelated nodes would only add demand.
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
			uint16_t visible_mask = 0;
			uint64_t child_splats = 0;
			double child_cost = 0.0;
			bool children_ready = true;
			uint32_t missing_children[gs_hlod::kMaxChildren];
			uint32_t missing_count = 0;
			for (uint32_t j = 0; j < n.child_count; ++j) {
				const uint32_t child = n.first_child + j;
				if (++out.visited_nodes > work_limit) { return fail(BOOTSTRAP_EXHAUSTED); }
				if (!visible(child)) { continue; }
				visible_mask |= uint16_t(1u << j);
				child_pages += pages_of(child); // Missing children and resident siblings share the same transition.
				if (!stale(child) && !available(child)) { children_ready = false; missing_children[missing_count++] = child; continue; }
				const double c = cost(child);
				if (!std::isfinite(c)) { return fail(BOOTSTRAP_ARITHMETIC); }
				children[visible_children] = child;
				child_costs[visible_children++] = c;
				child_splats += p_data.nodes[child].topology.payload_count;
				child_cost += c;
			}
			if (!transition_fits(parent, child_pages)) {
				if (!children_ready) { missed(parent, false); }
				continue; // Keep this complete parent and try the next bootstrap candidate, as for the node cap.
			}
			if (!children_ready) {
				// Parent stays; no partial replacement. The bootstrap needs this expansion, so every missing
				// visible child is demanded together (a whole transition, never a subset).
				missing_residency = true;
				const bool demanded = push_group(missing_children, missing_count,
						{ parent, n.first_child, 0, visible_mask, TRANSITION_BOOTSTRAP });
				missed(parent, demanded);
				if (demanded) {
					reserve_transition(parent, child_pages);
					for (uint32_t j = 0; j < n.child_count; ++j) {
						const uint32_t child = n.first_child + j;
						if (visible(child)) { report_pages(child, BOOTSTRAP_PAGE_ACCOUNTED); }
					}
					out.bootstrap_bypass_over_budget |= out.closure_measured && out.demanded_closure_pages > out.page_budget;
				}
				continue;
			}
			const uint32_t next_count = out.selected_count - 1 + visible_children;
			if (next_count > p_request.node_budget) { refused(parent); continue; } // Node cap refuses this expansion only.
			const double next_cost = MAX(0.0, out.estimated_records - cost(parent)) + child_cost;
			if (!std::isfinite(next_cost)) { return fail(BOOTSTRAP_ARITHMETIC); }
			const uint32_t removed = selected_position[parent];
			const uint32_t last = selected[--out.selected_count];
			selected.write[removed] = last;
			selected_position.write[last] = removed;
			if (stale(parent)) { --stale_selected; }
			for (uint32_t j = 0; j < visible_children; ++j) {
				report_pages(children[j], BOOTSTRAP_PAGE_ACCOUNTED);
				selected.write[out.selected_count] = children[j];
				selected_position.write[children[j]] = out.selected_count++;
				if (stale(children[j])) { ++stale_selected; }
				push_expandable(children[j], child_costs[j]);
			}
			out.selected_splats = out.selected_splats - n.payload_count + child_splats;
			out.estimated_records = next_cost;
			closure_pages += child_pages;
			out.bootstrap_bypass_over_budget |= out.closure_measured && out.demanded_closure_pages > out.page_budget;
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
	// Cross-fade (ADR 6.7): complete groups inside their band are deferred, in pop order, and admitted as fade pairs
	// after the walk. They are stored at the back of the heap storage: each is a popped member of the cut and every
	// heap entry is an unpopped one, so heap_count + deferred <= cut size <= node count and the two never meet.
	uint32_t deferred = 0;
	while (heap_count != 0) {
		std::pop_heap(heap.ptrw(), heap.ptrw() + heap_count, lower_priority);
		const uint32_t parent = heap[--heap_count].node;
		const auto &n = p_data.nodes[parent].topology;
		uint32_t children[gs_hlod::kMaxChildren];
		uint32_t visible_children = 0;
		uint16_t visible_mask = 0;
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
			visible_mask |= uint16_t(1u << j);
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
		if (complete && p_request.crossfade && error(parent) < kRefineBandFactor * p_request.tau_pixels) {
			// Inside the band: defer before any cap or page predicate is charged; the pair is admitted, or refused,
			// after every full refinement (below). A missing group takes the ordinary demand path instead.
			if (heap_count >= count - deferred) { out.status = INVALID; out.selected_count = 0; return out; } // Unreachable (see above).
			heap.write[count - 1 - deferred++] = { parent, tie_of(parent), error(parent) };
			continue;
		}
		const bool pages_fit = transition_fits(parent, child_pages);
		if (!complete) {
			// Residency miss: the parent stays and the walk continues (unchanged). The whole transition is
			// demanded only when it would fit every cap together with the transitions already demanded, so no
			// bandwidth goes to children whose replacement the budget would refuse once the higher-priority
			// ones are resident (the walk stops at the first refusal).
			const int64_t d_splats = int64_t(child_splats) - int64_t(n.payload_count);
			const int64_t d_count = int64_t(visible_children) - 1;
			const double d_cost = child_cost - cost(parent);
			bool demanded = false;
			const bool other_caps_fit = int64_t(out.selected_splats) + plan_splats + d_splats <= int64_t(p_request.splat_budget) &&
					int64_t(out.selected_count) + plan_count + d_count <= int64_t(p_request.node_budget) &&
					pages_fit;
			const double planned_cost = MAX(0.0, out.estimated_records + plan_cost + d_cost);
			if (other_caps_fit && planned_cost <= record_cap) {
				demanded = push_group(missing_children, missing_count,
						{ parent, n.first_child, 0, visible_mask, TRANSITION_REFINEMENT });
				if (demanded) {
					reserve_transition(parent, child_pages);
					for (uint32_t j = 0; j < visible_children; ++j) { report_pages(children[j], 0); }
				}
				plan_splats += d_splats;
				plan_count += d_count;
				plan_cost += d_cost;
			} else {
				// #1472: when only the record cap keeps this group from being requested, it never becomes resident, so it
				// never shows as a refused refinement either: name the demand here.
				if (other_caps_fit) { out.record_demand = MAX(out.record_demand, planned_cost); }
				refused(parent);
			}
			missed(parent, demanded);
			continue;
		}
		const bool selection_cap_refused = next_splats > p_request.splat_budget || next_count > p_request.node_budget || next_cost > record_cap;
		if (!pages_fit || selection_cap_refused) {
			// #1472: refused by the record cap alone (the splats and nodes fit).
			if (pages_fit && next_splats <= p_request.splat_budget && next_count <= p_request.node_budget) {
				out.record_demand = MAX(out.record_demand, next_cost);
			}
			out.budget_bound |= selection_cap_refused; // Preserve the comparator's splat/node/record-frame semantics.
			refused(parent);
			break; // First ready-group cap refusal stops the priority walk, including pages. Only accepted steps stamp history.
		}
		closure_pages += child_pages;
		const uint32_t removed = selected_position[parent];
		const uint32_t last = selected[--out.selected_count];
		selected.write[removed] = last;
		selected_position.write[last] = removed;
		for (uint32_t j = 0; j < visible_children; ++j) {
			report_pages(children[j], RESIDENT_PAGE_ACCOUNTED);
			selected.write[out.selected_count] = children[j];
			selected_position.write[children[j]] = out.selected_count++;
			enqueue(children[j]);
		}
		out.resident_refinement_bypass_over_budget |= out.closure_measured && out.demanded_closure_pages > out.page_budget;
		if (previous != 0 && refined[parent] == previous) {
			const double e = error(parent);
			if (e > p_request.tau_pixels && e <= kRefineBandFactor * p_request.tau_pixels) {
				diagnostics.write[parent] |= HYSTERESIS_HELD;
				++out.hysteresis_held_count;
			}
		}
		refined.write[parent] = epoch;
		out.selected_splats = next_splats;
		out.estimated_records = next_cost;
	}
	// Fade pairs (ADR 6.7), after every full refinement and in the walk's priority (pop) order. A pair keeps its parent
	// drawn, so it charges the children on top of the current cut; the first pair that does not fit every cap stops the
	// rest (the walk's first-refusal rule), and its parent is drawn alone.
	for (uint32_t k = 0; k < deferred; ++k) {
		const uint32_t parent = heap[count - 1 - k].node;
		const double e = heap[count - 1 - k].priority;
		const auto &n = p_data.nodes[parent].topology;
		uint32_t children[gs_hlod::kMaxChildren];
		uint32_t visible_children = 0;
		uint64_t child_splats = 0, child_pages = 0;
		double child_cost = 0.0;
		for (uint32_t j = 0; j < n.child_count; ++j) {
			const uint32_t child = n.first_child + j;
			if (!visible(child)) { continue; }
			children[visible_children++] = child;
			child_splats += p_data.nodes[child].topology.payload_count;
			child_pages += pages_of(child);
			child_cost += cost(child);
		}
		const uint64_t next_splats = out.selected_splats + child_splats;
		const uint32_t next_count = out.selected_count + visible_children;
		const double next_cost = out.estimated_records + child_cost;
		if (!std::isfinite(next_cost)) { out.status = INVALID; out.selected_count = 0; return out; }
		const bool selection_cap_refused = next_splats > p_request.splat_budget || next_count > p_request.node_budget || next_cost > record_cap;
		if (selection_cap_refused || !transition_fits(parent, child_pages)) {
			out.budget_bound |= selection_cap_refused;
			out.fade_refused = deferred - k;
			refused(parent);
			break;
		}
		const double t = (e - p_request.tau_pixels) / ((kRefineBandFactor - 1.0) * p_request.tau_pixels);
		// Overlapping weights (fade_cover): the children come in to full before the parent starts to leave, so the pair
		// never dips below one opaque layer (a plain t / 1 - t blend of two opaque layers is ~75% opaque mid-fade).
		const float child_fade = fade_cover(t);
		closure_pages += child_pages;
		fade.write[parent] = fade_cover(1.0 - t);
		diagnostics.write[parent] |= FADING;
		for (uint32_t j = 0; j < visible_children; ++j) {
			report_pages(children[j], RESIDENT_PAGE_ACCOUNTED);
			selected.write[out.selected_count] = children[j];
			selected_position.write[children[j]] = out.selected_count++;
			fade.write[children[j]] = child_fade;
			diagnostics.write[children[j]] |= FADING;
		}
		out.resident_refinement_bypass_over_budget |= out.closure_measured && out.demanded_closure_pages > out.page_budget;
		refined.write[parent] = epoch;
		out.selected_splats = next_splats;
		out.estimated_records = next_cost;
		++out.fade_pairs;
	}
	// Stable publication order, independent of heap storage and swap-removal order.
	std::sort(selected.ptrw(), selected.ptrw() + out.selected_count);
	if (p_data.has_edits()) {
		// Defence in depth: whatever path produced the cut, a stale ancestor in it is a stale coarse representation
		// of an edited region and must never be published (ADR 14.8 stop condition).
		for (uint32_t i = 0; i < out.selected_count; ++i) {
			if (stale(selected[i])) {
				out.status = INVALID;
				out.selected_count = 0;
				out.selected_splats = 0;
				out.estimated_records = 0.0;
				return out;
			}
		}
	}
	out.status = out.selected_count ? COMPLETE : EMPTY;
	if (out.status == COMPLETE && out.selected_count == 1 && selected[0] == 0 && reserve_blocked) {
		out.placement_reason = RESERVE_ROOT_ONLY;
	}
	return out;
}

// S6b temporal fade (CutRequest::fade_frames). Runs after a COMPLETE or EMPTY cut; owner thread, no allocation.
// Notation: live = the cut of this selection with its band fades (weights in fade[] where FADING, else 1); shown = what
// the previous selection drew (drawn_stamp, shown, prev_drawn). A group is a region (the subtree of its root) drawn as
// fade_cover(t) x live + fade_cover(1 - t) x old, old being the configuration the region showed before it started to
// change, frozen in the group's members. Per leaf each configuration covers once, so the blend covers between once and
// twice and never less: no hole, and no see-through dip.
void CutHistory::apply_temporal_fade(const GaussianSplatHlodRuntimeData &p_data, const CutRequest &p_request, uint64_t p_previous,
		Result &r_out) {
	const uint32_t count = p_data.nodes.size();
	const uint32_t frames = p_request.fade_frames;
	const uint32_t now = uint32_t(epoch);
	const uint32_t prev_tag = p_previous != 0 ? uint32_t(p_previous & 0x7fffffffu) : 0x80000000u; // Unmatchable when none.
	auto shown_last = [&](uint32_t p_node) { return (drawn_stamp[p_node] >> 1) == prev_tag; };
	auto live_last = [&](uint32_t p_node) { return shown_last(p_node) && (drawn_stamp[p_node] & 1u); };
	auto parent_of = [&](uint32_t p_node) {
		const uint32_t parent = p_data.nodes[p_node].topology.parent;
		return parent < count && parent != p_node ? parent : UINT32_MAX;
	};
	const uint32_t live_count = r_out.status == COMPLETE ? r_out.selected_count : 0u;
	for (uint32_t i = 0; i < live_count; ++i) { selected_position.write[selected[i]] = i; }
	auto in_live = [&](uint32_t p_node) {
		const uint32_t pos = selected_position[p_node];
		return pos < live_count && selected[pos] == p_node;
	};
	auto live_weight = [&](uint32_t p_node) { return diagnostics[p_node] & FADING ? fade[p_node] : 1.0f; };
	auto age_t = [&](const FadeGroup &p_group) { return float(MIN(1.0, double(now - p_group.start + 1u) / double(frames))); };
	// group_mark: low bits = 1 + group index of the groups of the previous selection; CANDIDATE and FINAL are scratch
	// for this pass and are cleared before it returns.
	constexpr uint32_t CANDIDATE = 0x80000000u, FINAL = 0x40000000u, SUBSUMED = 0x20000000u, INDEX = 0x1fffffffu;
	// 1. Changed nodes -> candidate roots. Arrival: live now, not live before; its root is the nearest ancestor drawn
	// before (a refinement, or a band fade starting). Departure: live before, not live now; its root is the nearest
	// live ancestor (a merge). A change with neither (frustum entry or exit) starts nothing. Candidates use the heap
	// storage, which select_cut no longer needs; each node is listed at most once (CANDIDATE), so count bounds them.
	uint32_t candidate_count = 0;
	auto add_candidate = [&](uint32_t p_node) {
		if (group_mark[p_node] & CANDIDATE) { return; }
		group_mark.write[p_node] |= CANDIDATE;
		heap.write[candidate_count++].node = p_node;
	};
	for (uint32_t i = 0; i < live_count; ++i) {
		const uint32_t node = selected[i];
		if (live_last(node)) { continue; }
		for (uint32_t a = parent_of(node); a != UINT32_MAX; a = parent_of(a)) {
			if (shown_last(a)) { add_candidate(a); break; }
		}
	}
	for (uint32_t i = 0; i < prev_drawn_count; ++i) {
		const uint32_t node = prev_drawn[i];
		if (!live_last(node) || in_live(node)) { continue; }
		for (uint32_t a = parent_of(node); a != UINT32_MAX; a = parent_of(a)) {
			if (in_live(a)) { add_candidate(a); break; }
		}
	}
	// 2. Final roots: the highest candidate at or above each candidate. A change inside a region that is already fading
	// starts nothing: it is part of that region's live configuration, drawn at fade_cover(t) over its old one, which
	// still covers fully for the first half of the fade. A new region over fading ones subsumes them (below). Final roots
	// go to the `tie` field from the back of the heap storage; the candidates use the `node` field from the front, so
	// even overlapping slots never clobber each other.
	uint32_t final_count = 0;
	for (uint32_t c = 0; c < candidate_count; ++c) {
		uint32_t top = heap[c].node;
		for (uint32_t a = top; a != UINT32_MAX; a = parent_of(a)) {
			if (group_mark[a] & (CANDIDATE | INDEX)) { top = a; }
		}
		if (group_mark[top] & INDEX) { continue; } // Inside (or at the root of) a fading region.
		if (!(group_mark[top] & FINAL)) {
			group_mark.write[top] |= FINAL;
			heap.write[count - 1 - final_count++].tie = top;
		}
	}
	auto final_root_of = [&](uint32_t p_node) {
		uint32_t found = UINT32_MAX;
		for (uint32_t a = p_node; a != UINT32_MAX; a = parent_of(a)) {
			if (group_mark[a] & FINAL) { found = a; }
		}
		return found;
	};
	// Groups under a new root are subsumed: their old configuration (not their partial blend) becomes part of the new
	// group's, so the frozen configuration of every region covers each leaf exactly once.
	for (uint32_t g = 0; g < group_count; ++g) {
		if (final_root_of(groups[g].root) != UINT32_MAX) { group_mark.write[groups[g].root] |= SUBSUMED; }
	}
	auto inside_subsumed = [&](uint32_t p_node) {
		for (uint32_t a = p_node; a != UINT32_MAX; a = parent_of(a)) {
			if (group_mark[a] & SUBSUMED) { return true; }
		}
		return false;
	};
	// The final root of every previously drawn node outside subsumed regions, once, in the heap storage's unused
	// `priority` field (prev_drawn_count <= count); -1 for none.
	for (uint32_t i = 0; final_count != 0 && i < prev_drawn_count; ++i) {
		const uint32_t root = inside_subsumed(prev_drawn[i]) ? UINT32_MAX : final_root_of(prev_drawn[i]);
		heap.write[i].priority = root == UINT32_MAX ? -1.0 : double(root);
	}
	// 3. Next group list: surviving groups (not subsumed, not finished) keep their members; new groups capture the shown
	// configuration of their region now. Bounded: a group that does not fit the bookkeeping snaps.
	uint32_t next_group_count = 0, next_member_count = 0;
	const uint32_t group_cap = groups.size(), member_cap = members.size();
	for (uint32_t g = 0; g < group_count; ++g) {
		const FadeGroup &group = groups[g];
		if (final_root_of(group.root) != UINT32_MAX) { continue; } // Subsumed: its members move to the new group below.
		if (age_t(group) >= 1.0f) { continue; } // Finished: the region draws its live configuration.
		FadeGroup kept = group;
		kept.first_member = next_member_count;
		for (uint32_t m = 0; m < group.member_count; ++m) { next_members.write[next_member_count++] = members[group.first_member + m]; }
		next_groups.write[next_group_count++] = kept;
	}
	for (uint32_t f = 0; f < final_count; ++f) {
		const uint32_t root = heap[count - 1 - f].tie;
		if (next_group_count >= group_cap) { ++r_out.fade_snapped; continue; }
		FadeGroup group;
		group.root = root;
		group.start = now;
		group.first_member = next_member_count;
		bool fits = true;
		for (uint32_t i = 0; i < prev_drawn_count && fits; ++i) {
			const uint32_t node = prev_drawn[i];
			if (heap[i].priority != double(root)) { continue; }
			if (next_member_count >= member_cap) { fits = false; break; }
			next_members.write[next_member_count++] = { node, shown[node] };
		}
		for (uint32_t g = 0; g < group_count && fits; ++g) {
			const FadeGroup &old = groups[g];
			if (!(group_mark[old.root] & SUBSUMED) || final_root_of(old.root) != root) { continue; }
			for (uint32_t m = 0; m < old.member_count; ++m) {
				if (next_member_count >= member_cap) { fits = false; break; }
				next_members.write[next_member_count++] = members[old.first_member + m];
			}
		}
		group.member_count = next_member_count - group.first_member;
		if (!fits) { next_member_count = group.first_member; ++r_out.fade_snapped; continue; }
		if (group.member_count == 0) { continue; } // Nothing was shown there: nothing to fade from.
		next_groups.write[next_group_count++] = group;
	}
	// Scratch marks off, and the index marks of the previous groups (rebuilt below for the admitted ones).
	for (uint32_t c = 0; c < candidate_count; ++c) { group_mark.write[heap[c].node] = 0; }
	for (uint32_t f = 0; f < final_count; ++f) { group_mark.write[heap[count - 1 - f].tie] = 0; }
	for (uint32_t g = 0; g < group_count; ++g) { group_mark.write[groups[g].root] = 0; }
	// 4. Admission from the headroom, oldest group first (the list keeps creation order): the old nodes a group draws on
	// top of the cut must fit the caps together with the cut and the groups admitted before it.
	auto node_cost = [&](uint32_t p_node) {
		const auto &n = p_data.nodes[p_node];
		const double dx = double(n.bounds_center.x) - p_request.camera.x;
		const double dy = double(n.bounds_center.y) - p_request.camera.y;
		const double dz = double(n.bounds_center.z) - p_request.camera.z;
		const double surface = MAX(std::sqrt(dx * dx + dy * dy + dz * dz) - double(n.topology.radius), 1.0e-6);
		const double d = MAX(surface, kCostInsideDistanceFraction * double(n.topology.radius));
		return double(n.topology.payload_count) + n.footprint * p_request.focal_pixels * p_request.focal_pixels /
				(d * d * p_request.tile_area);
	};
	auto usable = [&](const FadeMember &p_member, float p_t) {
		const bool available = !p_request.resident || p_request.resident[p_member.node] != 0;
		return available && !p_data.is_stale(p_member.node) && fade_cover(1.0 - p_t) * p_member.weight >= kFadeMinWeight;
	};
	const double record_cap = kRecordAdmissionFactor * p_request.record_budget;
	uint64_t splats = r_out.selected_splats;
	uint32_t nodes = live_count;
	double records = r_out.estimated_records;
	group_count = 0;
	member_count = 0;
	for (uint32_t g = 0; g < next_group_count; ++g) {
		FadeGroup group = next_groups[g];
		const float t = age_t(group);
		uint64_t extra_splats = 0;
		uint32_t extra_nodes = 0;
		double extra_records = 0.0;
		for (uint32_t m = 0; m < group.member_count; ++m) {
			const FadeMember &member = next_members[group.first_member + m];
			if (in_live(member.node) || !usable(member, t)) { continue; }
			extra_splats += p_data.nodes[member.node].topology.payload_count;
			extra_records += node_cost(member.node);
			++extra_nodes;
		}
		if (splats + extra_splats > p_request.splat_budget || nodes + extra_nodes > p_request.node_budget ||
				!std::isfinite(records + extra_records) || records + extra_records > record_cap) {
			++r_out.fade_snapped; // Switched at once: the region draws its live configuration.
			continue;
		}
		splats += extra_splats;
		nodes += extra_nodes;
		records += extra_records;
		r_out.fade_extra_splats += extra_splats;
		const uint32_t first = member_count;
		for (uint32_t m = 0; m < group.member_count; ++m) { members.write[member_count++] = next_members[group.first_member + m]; }
		group.first_member = first;
		groups.write[group_count++] = group;
		group_mark.write[group.root] = group_count; // 1 + index.
	}
	r_out.fade_groups = group_count;
	// 5. Drawn weights. A live node inside a group region draws fade_cover(t) x its live weight, an old member
	// fade_cover(1 - t) x its frozen weight; a node in both takes the larger. Every weight stays in [0, 1].
	auto group_of = [&](uint32_t p_node) {
		for (uint32_t a = p_node; a != UINT32_MAX; a = parent_of(a)) {
			if (group_mark[a] & INDEX) { return (group_mark[a] & INDEX) - 1u; }
		}
		return UINT32_MAX;
	};
	// Coverage (thread 4677949866 on #1542). The live side fades in at fade_cover(t) only where the group's old
	// configuration still draws: a live node is covered when an old member is its ancestor-or-self, or when every
	// branch under it that is visible now reaches an old member. The old configuration of a frustum-filtered cut holds
	// only what was visible; a branch the camera exposes mid-fade is covered by no member, and a live node over it keeps
	// its full live weight there (the old members still fade out over it, so coverage stays between once and twice).
	// A member dropped below kFadeMinWeight still counts as cover (it is negligible, not missing); a non-resident or
	// stale one does not draw and does not. Marks use spare high bits of group_mark (clean here), cleared below.
	constexpr uint32_t OLD_MEMBER = CANDIDATE, MEMBER_BELOW = FINAL;
	auto drawable = [&](const FadeMember &p_member) {
		return (!p_request.resident || p_request.resident[p_member.node] != 0) && !p_data.is_stale(p_member.node);
	};
	auto mark_members = [&](bool p_set) {
		for (uint32_t g = 0; g < group_count; ++g) {
			const FadeGroup &group = groups[g];
			for (uint32_t m = 0; m < group.member_count; ++m) {
				const FadeMember &member = members[group.first_member + m];
				if (!drawable(member)) { continue; }
				group_mark.write[member.node] = p_set ? (group_mark[member.node] | OLD_MEMBER) : (group_mark[member.node] & ~OLD_MEMBER);
				// The chain up to the group root (members lie in its subtree); shared tails stop the walk.
				for (uint32_t a = member.node; a != group.root;) {
					a = parent_of(a);
					if (a == UINT32_MAX || bool(group_mark[a] & MEMBER_BELOW) == p_set) { break; }
					group_mark.write[a] = p_set ? (group_mark[a] | MEMBER_BELOW) : (group_mark[a] & ~MEMBER_BELOW);
				}
			}
		}
	};
	auto visible = [&](uint32_t p_node) {
		const auto &n = p_data.nodes[p_node];
		if (!p_request.frustum_enabled) { return true; }
		for (const Plane &plane : p_request.frustum) {
			if (plane.distance_to(n.bounds_center) > n.topology.radius) { return false; }
		}
		return true;
	};
	auto covered = [&](uint32_t p_node, uint32_t p_root) {
		for (uint32_t a = p_node; a != UINT32_MAX; a = parent_of(a)) {
			if (group_mark[a] & OLD_MEMBER) { return true; }
			if (a == p_root) { break; }
		}
		// Depth-first over the visible subtree, in the heap storage (free after step 3; one entry per node at most).
		uint32_t stack = 0;
		heap.write[stack++].node = p_node;
		while (stack != 0) {
			const uint32_t n = heap[--stack].node;
			if (group_mark[n] & OLD_MEMBER) { continue; }
			if (!(group_mark[n] & MEMBER_BELOW)) { return false; } // A visible branch no old member reaches.
			const auto &topology = p_data.nodes[n].topology;
			for (uint32_t j = 0; j < topology.child_count; ++j) {
				const uint32_t child = topology.first_child + j;
				if (child < count && visible(child)) { heap.write[stack++].node = child; }
			}
		}
		return true;
	};
	mark_members(true);
	for (uint32_t i = 0; i < live_count; ++i) {
		const uint32_t node = selected[i];
		const uint32_t g = group_of(node);
		const float t = g == UINT32_MAX ? 1.0f : age_t(groups[g]);
		const bool blend = t < 1.0f && covered(node, groups[g].root);
		fade.write[node] = (blend ? fade_cover(t) : 1.0f) * live_weight(node);
		diagnostics.write[node] |= FADING;
	}
	mark_members(false);
	uint32_t drawn_count = live_count;
	for (uint32_t g = 0; g < group_count; ++g) {
		const FadeGroup &group = groups[g];
		const float t = age_t(group);
		for (uint32_t m = 0; m < group.member_count; ++m) {
			const FadeMember &member = members[group.first_member + m];
			if (!usable(member, t)) { continue; }
			const float w = fade_cover(1.0 - t) * member.weight;
			if (in_live(member.node)) {
				// The same node in both configurations: it is one representation, drawn at the larger of its weights.
				fade.write[member.node] = MAX(fade[member.node], w);
				continue;
			}
			// Distinct from every live node and from the other groups' members (disjoint regions): drawn <= count.
			selected.write[drawn_count++] = member.node;
			fade.write[member.node] = w;
			diagnostics.write[member.node] |= FADING;
		}
	}
	r_out.selected_count = drawn_count;
	r_out.selected_splats = splats;
	r_out.estimated_records = records;
	if (r_out.status == EMPTY && drawn_count != 0) { r_out.status = COMPLETE; }
	// 6. Remember what was drawn: entries [0, live_count) are the live cut, the rest old members.
	const uint32_t tag = (now & 0x7fffffffu) << 1;
	for (uint32_t i = 0; i < drawn_count; ++i) {
		const uint32_t node = selected[i];
		drawn_stamp.write[node] = tag | (i < live_count ? 1u : 0u);
		shown.write[node] = fade[node];
		prev_drawn.write[i] = node;
	}
	prev_drawn_count = drawn_count;
	std::sort(selected.ptrw(), selected.ptrw() + drawn_count); // Stable publication order.
}
}
