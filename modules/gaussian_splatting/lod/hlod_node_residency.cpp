#include "hlod_node_residency.h"

#include "../core/gs_vector_alloc.h"
#include <algorithm>

namespace gs_hlod_runtime {

bool make_node_infos(const GaussianSplatHlodRuntimeData &p_data, Vector<NodeResidency::NodeInfo> &r_infos) {
	const uint32_t count = p_data.nodes.size();
	if (count == 0 || !gs_resize_or_fail(r_infos, count, "HLOD residency node table")) {
		return false;
	}
	for (uint32_t i = 0; i < count; ++i) {
		const auto &topology = p_data.nodes[i].topology;
		NodeResidency::NodeInfo &info = r_infos.write[i];
		info.splat_count = topology.payload_count;
		info.parent = topology.parent == gs_hlod::kNoParent ? UINT32_MAX : topology.parent;
		info.height = topology.height;
		info.stale = p_data.is_stale(i);
	}
	return true;
}

bool NodeResidency::reset(const Vector<NodeInfo> &p_nodes, const Config &p_config, uint64_t p_world_generation, uint64_t p_device_generation) {
	nodes.clear();
	mask.clear();
	retiring.clear();
	candidates.clear();
	evicted_log.clear();
	cancelled_generations.clear();
	cancelled_next = 0;
	allocator.clear();
	totals = Stats();
	pages_retiring = 0;
	edit_staged = false;
	staged_revision = committed_revision = 0;
	staged_leaves.clear();
	staged_splats.clear();
	staged_stale.clear();
	staged_required.clear();
	shadow_alloc_pending = false;
	requests_this_frame = evictions_this_frame = 0;
	current_frame = 0;
	request_serial = alloc_serial = 0;
	cfg = p_config;
	world_gen = p_world_generation;
	device_gen = p_device_generation;
	const uint32_t count = p_nodes.size();
	if (count == 0 || cfg.page_capacity == 0 || cfg.page_splats == 0) {
		return false;
	}
	Vector<Node> next;
	Vector<uint8_t> next_mask;
	if (!gs_resize_or_fail(next, count, "HLOD residency nodes") || !gs_resize_or_fail(next_mask, count, "HLOD residency mask")) {
		return false;
	}
	next_mask.fill(0);
	for (uint32_t i = 0; i < count; ++i) {
		const NodeInfo &info = p_nodes[i];
		if (info.splat_count == 0 || (info.parent != UINT32_MAX && info.parent >= i)) {
			return false; // Parents precede children (validated tree shape).
		}
		Node &n = next.write[i];
		n = Node();
		n.parent = info.parent;
		n.height = info.height;
		n.stale = info.stale;
		n.pages = (info.splat_count + cfg.page_splats - 1u) / cfg.page_splats;
	}
	nodes = std::move(next);
	mask = std::move(next_mask);
	committed_revision = cfg.committed_edit_revision;
	allocator.reset(cfg.page_capacity);
	return true;
}

uint64_t NodeResidency::make_alloc_key(uint32_t p_node) {
	return (++alloc_serial << 32) | uint64_t(p_node);
}

void NodeResidency::begin_frame(uint64_t p_frame) {
	current_frame = p_frame;
	requests_this_frame = 0;
	evictions_this_frame = 0;
	for (uint32_t i = 0; i < retiring.size();) {
		if (retiring[i].retire_frame <= current_frame) {
			allocator.release_slot(retiring[i].alloc_key);
			pages_retiring -= retiring[i].pages;
			retiring[i] = retiring[retiring.size() - 1];
			retiring.resize(retiring.size() - 1);
		} else {
			++i;
		}
	}
}

void NodeResidency::protect_drawn(const uint32_t *p_nodes, uint32_t p_count) {
	for (uint32_t i = 0; i < p_count; ++i) {
		uint32_t n = p_nodes[i];
		bool first = true;
		while (n < uint32_t(nodes.size())) {
			Node &node = nodes.write[n];
			if (!first && node.protected_frame == current_frame) {
				break; // The rest of the chain was already marked by an earlier node this frame.
			}
			node.protected_frame = current_frame;
			node.last_drawn_frame = current_frame;
			first = false;
			n = node.parent;
		}
	}
}

bool NodeResidency::is_protected(uint32_t p_node) const {
	if (p_node >= uint32_t(nodes.size())) {
		return false;
	}
	const Node &n = nodes[p_node];
	// In-flight work is protected by construction: only RESIDENT nodes are ever evictable.
	// A stale node (package F1) is neither pinned nor protected: it is no valid fallback. A replacement child of a staged
	// edit is held until the edit commits or is cancelled.
	if (n.stale) {
		return false;
	}
	return n.protected_frame == current_frame || (cfg.pin_root && p_node == 0) || n.state == LOADING || n.state == UPLOADING || n.edit_hold;
}

void NodeResidency::release_run_now(Node &r_node) {
	if (r_node.alloc_key != 0) {
		allocator.release_slot(r_node.alloc_key);
		r_node.alloc_key = 0;
	}
}

void NodeResidency::retire_run(Node &r_node) {
	if (r_node.alloc_key == 0) {
		return;
	}
	if (cfg.retire_delay_frames == 0) {
		release_run_now(r_node);
		return;
	}
	Retiring r;
	r.alloc_key = r_node.alloc_key;
	r.first_page = r_node.first_page;
	r.pages = r_node.pages;
	r.retire_frame = current_frame + cfg.retire_delay_frames;
	retiring.push_back(r);
	pages_retiring += r.pages;
	r_node.alloc_key = 0;
}

bool NodeResidency::evict(uint32_t p_node) {
	if (p_node >= uint32_t(nodes.size())) {
		return false;
	}
	Node &n = nodes.write[p_node];
	if (n.state != RESIDENT || is_protected(p_node)) {
		return false;
	}
	mask.write[p_node] = 0;
	n.state = ABSENT;
	n.request_generation = 0; // Any ticket still around for this node is dead.
	retire_run(n);
	evicted_log.push_back(p_node);
	++totals.evicted_total;
	++evictions_this_frame;
	return true;
}

uint32_t NodeResidency::evict_for(uint32_t p_pages, uint32_t p_exclude_parent) {
	uint32_t evicted = 0;
	if (allocator.get_free_page_count() >= p_pages) {
		// Pages may be free but fragmented; eviction below only runs when the total is short. A fragmented
		// pool is compaction's job (D2), not eviction's.
		return 0;
	}
	candidates.clear();
	for (uint32_t i = 0; i < uint32_t(nodes.size()); ++i) {
		const Node &n = nodes[i];
		if (n.state != RESIDENT || is_protected(i) || i == p_exclude_parent || (p_exclude_parent != UINT32_MAX && n.parent == p_exclude_parent)) {
			continue;
		}
		candidates.push_back(i);
	}
	// Evicting must pay for the request: when every reclaimable page together still cannot hold the group the
	// request is skipped and nothing is evicted (a futile eviction only forces its victims to be re-requested).
	uint64_t reclaimable = uint64_t(allocator.get_free_page_count()) + pages_retiring;
	for (uint32_t i = 0; i < candidates.size(); ++i) {
		reclaimable += nodes[candidates[i]].pages;
	}
	if (reclaimable < p_pages) {
		return 0;
	}
	// Least recently drawn first, then the finest level (leaves) before interiors, then node index.
	std::sort(candidates.ptr(), candidates.ptr() + candidates.size(), [this](uint32_t a, uint32_t b) {
		const Node &na = nodes[a];
		const Node &nb = nodes[b];
		if (na.last_drawn_frame != nb.last_drawn_frame) { return na.last_drawn_frame < nb.last_drawn_frame; }
		if (na.height != nb.height) { return na.height < nb.height; }
		return a < b;
	});
	// Retiring runs count toward the target: they become free on their own.
	for (uint32_t i = 0; i < candidates.size() && allocator.get_free_page_count() + pages_retiring < p_pages &&
			evictions_this_frame < cfg.evictions_per_frame; ++i) {
		if (evict(candidates[i])) {
			++evicted;
		}
	}
	return evicted;
}

uint32_t NodeResidency::evict_window(uint32_t p_pages, uint32_t p_exclude_parent) {
	const uint32_t capacity = cfg.page_capacity;
	if (p_pages == 0 || p_pages > capacity || evictions_this_frame >= cfg.evictions_per_frame) {
		return 0;
	}
	const uint32_t FREE_PAGE = UINT32_MAX, BLOCKED_PAGE = UINT32_MAX - 1;
	window_owner.resize(capacity);
	for (uint32_t i = 0; i < capacity; ++i) {
		window_owner[i] = FREE_PAGE;
	}
	for (uint32_t i = 0; i < uint32_t(nodes.size()); ++i) {
		const Node &n = nodes[i];
		if (n.alloc_key == 0) {
			continue;
		}
		const bool evictable = n.state == RESIDENT && !is_protected(i) && i != p_exclude_parent &&
				(p_exclude_parent == UINT32_MAX || n.parent != p_exclude_parent);
		for (uint32_t p = n.first_page; p < n.first_page + n.pages && p < capacity; ++p) {
			window_owner[p] = evictable ? i : BLOCKED_PAGE;
		}
	}
	for (uint32_t i = 0; i < retiring.size(); ++i) {
		for (uint32_t p = retiring[i].first_page; p < retiring[i].first_page + retiring[i].pages && p < capacity; ++p) {
			window_owner[p] = BLOCKED_PAGE;
		}
	}
	uint32_t best_start = UINT32_MAX, best_cost = UINT32_MAX;
	for (uint32_t s = 0; s + p_pages <= capacity; ++s) {
		uint32_t cost = 0;
		bool ok = true;
		uint32_t previous = FREE_PAGE;
		for (uint32_t p = s; p < s + p_pages; ++p) {
			const uint32_t owner = window_owner[p];
			if (owner == BLOCKED_PAGE) { ok = false; break; }
			if (owner != FREE_PAGE && owner != previous) { cost += nodes[owner].pages; }
			previous = owner;
		}
		if (ok && cost != 0 && cost < best_cost) {
			best_cost = cost;
			best_start = s;
		}
	}
	if (best_start == UINT32_MAX) {
		return 0;
	}
	uint32_t evicted = 0, previous = FREE_PAGE;
	for (uint32_t p = best_start; p < best_start + p_pages; ++p) {
		const uint32_t owner = window_owner[p];
		if (owner != FREE_PAGE && owner != previous && evict(owner)) {
			++evicted;
		}
		previous = owner;
	}
	return evicted;
}

NodeResidency::RequestResult NodeResidency::request(const uint32_t *p_demand, uint32_t p_count, LocalVector<Ticket> &r_tickets) {
	RequestResult out;
	// The selector can list a node twice (a bootstrap expansion and the refinement walk both reach it). One
	// node is one demand and gets at most one run: keep the first occurrence, in priority order.
	demand_unique.clear();
	++dedup_pass;
	for (uint32_t i = 0; i < p_count; ++i) {
		if (p_demand[i] < uint32_t(nodes.size()) && nodes[p_demand[i]].demand_pass != dedup_pass) {
			nodes.write[p_demand[i]].demand_pass = dedup_pass;
			nodes.write[p_demand[i]].last_demand_frame = current_frame;
			demand_unique.push_back(p_demand[i]);
		}
	}
	p_demand = demand_unique.ptr();
	p_count = demand_unique.size();
	// Requests nobody asked for lately are cancelled, so a camera that moved on does not keep loading.
	for (uint32_t i = 0; i < uint32_t(nodes.size()); ++i) {
		const Node &n = nodes[i];
		if (n.state == LOADING && current_frame > n.last_demand_frame + cfg.cancel_grace_frames) {
			cancel(i);
		}
	}
	uint32_t at = 0;
	while (at < p_count) {
		if (p_demand[at] >= uint32_t(nodes.size())) {
			++at;
			continue;
		}
		// One group = consecutive demand with the same parent (the root is its own group).
		const uint32_t parent = nodes[p_demand[at]].parent;
		uint32_t end = at + 1;
		while (parent != UINT32_MAX && end < p_count && p_demand[end] < uint32_t(nodes.size()) && nodes[p_demand[end]].parent == parent) {
			++end;
		}
		uint32_t need[16];
		uint32_t need_count = 0, total_pages = 0;
		bool blocked = false;
		for (uint32_t k = at; k < end; ++k) {
			const Node &n = nodes[p_demand[k]];
			if (n.stale) {
				blocked = true; // Package F1: a stale node is never loaded; the selector replaces it by its children.
			} else if (n.state == LOADING || n.state == UPLOADING || n.state == RESIDENT) {
				++out.satisfied;
			} else if (n.state == FAILED && (n.failures >= cfg.max_failures || current_frame < n.retry_after_frame)) {
				blocked = true; // A child cannot arrive now, so the parent's whole transition cannot complete.
			} else if (n.alloc_key != 0) {
				blocked = true; // Never give a node a second run: its first one would leak.
			} else if (need_count < 16) {
				need[need_count++] = p_demand[k];
				total_pages += n.pages;
			} else {
				blocked = true;
			}
		}
		const uint32_t group_begin = at;
		at = end;
		// A group that is complete or already on its way is wanted by a higher-priority refinement than anything
		// still to be placed this pass: its members (resident ones included) are protected from eviction by
		// lower-priority groups. Without this, two groups competing for one atlas evict each other's
		// half-finished children every frame and neither completes.
		auto want_group = [&]() {
			for (uint32_t k = group_begin; k < end; ++k) {
				nodes.write[p_demand[k]].protected_frame = current_frame;
			}
		};
		if (blocked) {
			++out.blocked_groups;
			continue;
		}
		if (need_count == 0) {
			want_group();
			continue;
		}
		if (requests_this_frame + need_count > cfg.max_requests_per_frame) {
			out.capped = true;
			break; // Priority order: lower-priority groups do not jump the queue.
		}
		// Largest runs first: they are the hardest to place, small ones fill the holes that remain.
		for (uint32_t a = 1; a < need_count; ++a) {
			const uint32_t v = need[a];
			uint32_t b = a;
			while (b > 0 && nodes[need[b - 1]].pages < nodes[v].pages) { need[b] = need[b - 1]; --b; }
			need[b] = v;
		}
		uint32_t failed_pages = 0;
		auto allocate_all = [&]() {
			for (uint32_t k = 0; k < need_count; ++k) {
				Node &n = nodes.write[need[k]];
				const uint64_t key = make_alloc_key(need[k]);
				uint32_t slot = 0;
				if (!allocator.allocate_slot(key, n.pages, slot)) {
					failed_pages = n.pages;
					for (uint32_t r = 0; r < k; ++r) { // Roll back: these runs were never used.
						release_run_now(nodes.write[need[r]]);
					}
					return false;
				}
				n.alloc_key = key;
				n.first_page = slot;
			}
			return true;
		};
		bool placed = allocate_all();
		if (!placed) {
			out.evicted += evict_for(total_pages, parent);
			placed = allocate_all();
		}
		// Enough pages free in total but no contiguous run: evict the cheapest unprotected neighbours of a hole.
		// A shortfall covered by retiring pages waits, and so does a group while a window retires.
		for (uint32_t round = 0; !placed && round < need_count && pages_retiring == 0 && uint64_t(allocator.get_free_page_count()) >= total_pages; ++round) {
			const uint32_t freed = evict_window(failed_pages, parent);
			if (freed == 0) { break; }
			out.evicted += freed;
			placed = allocate_all();
		}
		if (!placed) {
			++out.skipped_groups;
			++totals.skipped_groups_total;
			if (uint64_t(allocator.get_free_page_count()) >= total_pages) {
				++out.fragmented_groups;
			} else if (uint64_t(allocator.get_free_page_count()) + pages_retiring >= total_pages) {
				// It will fit as soon as the retiring runs are released. Until then the group still outranks
				// every lower-priority one: its half-finished children and the pages being freed for it stay.
				++out.pending_retire_groups;
				want_group();
			}
			continue; // Skipped, not waited for: the parent keeps being drawn.
		}
		want_group();
		for (uint32_t k = 0; k < need_count; ++k) {
			Node &n = nodes.write[need[k]];
			// An evicted node that is requested again in the same pass is no longer "evicted": clearing its GPU
			// metadata after the new payload landed would leave a resident node that draws nothing.
			for (uint32_t e = 0; e < evicted_log.size(); ++e) {
				if (evicted_log[e] == need[k]) {
					evicted_log[e] = evicted_log[evicted_log.size() - 1];
					evicted_log.resize(evicted_log.size() - 1);
					break;
				}
			}
			n.state = LOADING;
			n.request_generation = ++request_serial;
			Ticket t;
			t.node = need[k];
			t.first_page = n.first_page;
			t.page_count = n.pages;
			t.request_generation = n.request_generation;
			t.world_generation = world_gen;
			t.device_generation = device_gen;
			r_tickets.push_back(t);
			++out.requested;
			++requests_this_frame;
			++totals.requested_total;
		}
	}
	return out;
}

bool NodeResidency::ticket_current(const Ticket &p_ticket, State p_expected) const {
	return p_ticket.valid() && !p_ticket.shadow && p_ticket.world_generation == world_gen && p_ticket.device_generation == device_gen &&
			p_ticket.node < uint32_t(nodes.size()) && nodes[p_ticket.node].request_generation == p_ticket.request_generation &&
			nodes[p_ticket.node].state == p_expected;
}

bool NodeResidency::consume_cancelled(const Ticket &p_ticket) {
	if (!p_ticket.valid() || p_ticket.world_generation != world_gen || p_ticket.device_generation != device_gen) {
		return false;
	}
	for (uint32_t i = 0; i < cancelled_generations.size(); ++i) {
		if (cancelled_generations[i] == p_ticket.request_generation) {
			cancelled_generations[i] = 0; // Consumed: a second delivery of the same ticket is a duplicate, not a cancel.
			return true;
		}
	}
	return false;
}

void NodeResidency::count_rejected_completion(const Ticket &p_ticket) {
	if (consume_cancelled(p_ticket)) {
		++totals.late_dropped_cancelled_total;
	} else {
		++totals.late_dropped_total;
	}
}

bool NodeResidency::begin_upload(const Ticket &p_ticket) {
	if (!ticket_current(p_ticket, LOADING)) {
		count_rejected_completion(p_ticket);
		return false;
	}
	nodes.write[p_ticket.node].state = UPLOADING;
	return true;
}

bool NodeResidency::finish_upload(const Ticket &p_ticket, bool p_success) {
	if (!ticket_current(p_ticket, UPLOADING)) {
		count_rejected_completion(p_ticket);
		return false;
	}
	Node &n = nodes.write[p_ticket.node];
	if (p_success) {
		n.state = RESIDENT;
		n.failures = 0;
		n.last_drawn_frame = current_frame; // A fresh node is not the first eviction victim.
		mask.write[p_ticket.node] = 1;
		++totals.completed_total;
		return true;
	}
	n.state = FAILED;
	n.request_generation = 0;
	++n.failures;
	n.retry_after_frame = current_frame + uint64_t(cfg.retry_delay_frames) * n.failures;
	++totals.failed_total;
	retire_run(n); // The GPU may have partly written the run.
	return true;
}

bool NodeResidency::fail_load(const Ticket &p_ticket) {
	if (!ticket_current(p_ticket, LOADING)) {
		count_rejected_completion(p_ticket);
		return false;
	}
	Node &n = nodes.write[p_ticket.node];
	n.state = FAILED;
	n.request_generation = 0;
	++n.failures;
	n.retry_after_frame = current_frame + uint64_t(cfg.retry_delay_frames) * n.failures;
	++totals.failed_total;
	release_run_now(n); // Nothing reached the GPU.
	return true;
}

bool NodeResidency::cancel(uint32_t p_node) {
	if (p_node >= uint32_t(nodes.size())) {
		return false;
	}
	Node &n = nodes.write[p_node];
	if (n.state != LOADING && n.state != UPLOADING) {
		return false;
	}
	n.state = ABSENT;
	// Remember the killed generation so its late completion is told apart from a real loss.
	if (cancelled_generations.size() < CANCELLED_WINDOW) {
		cancelled_generations.push_back(n.request_generation);
	} else {
		cancelled_generations[cancelled_next] = n.request_generation;
		cancelled_next = (cancelled_next + 1) % CANCELLED_WINDOW;
	}
	n.request_generation = 0; // Kills the ticket: its late completion is dropped.
	retire_run(n);
	++totals.cancelled_total;
	return true;
}

bool NodeResidency::run(uint32_t p_node, uint32_t &r_first_page, uint32_t &r_page_count) const {
	if (p_node >= uint32_t(nodes.size())) {
		return false;
	}
	const Node &n = nodes[p_node];
	if (n.alloc_key == 0) {
		return false;
	}
	r_first_page = n.first_page;
	r_page_count = n.pages;
	return true;
}

bool NodeResidency::root_permanently_failed() const {
	return !nodes.is_empty() && nodes[0].state == FAILED && nodes[0].failures >= cfg.max_failures;
}

NodeResidency::Stats NodeResidency::stats() const {
	Stats s = totals;
	for (uint32_t i = 0; i < uint32_t(nodes.size()); ++i) {
		const Node &n = nodes[i];
		switch (n.state) {
			case ABSENT: ++s.absent; break;
			case LOADING: ++s.loading; s.pages_in_flight += n.pages; break;
			case UPLOADING: ++s.uploading; s.pages_in_flight += n.pages; break;
			case RESIDENT: ++s.resident; s.pages_resident += n.pages; break;
			case FAILED:
				++s.failed;
				if (n.failures >= cfg.max_failures) { ++s.permanently_failed; }
				break;
		}
	}
	for (uint32_t i = 0; i < uint32_t(nodes.size()); ++i) {
		const Node &n = nodes[i];
		if (n.stale) { ++s.stale_nodes; }
		if (n.shadow_state != Node::SHADOW_NONE) {
			s.pages_shadow += n.shadow_pages;
			s.pages_in_flight += n.shadow_pages;
		}
	}
	s.edit_staged = edit_staged;
	s.edit_waiting_for_space = edit_staged && shadow_alloc_pending;
	s.committed_edit_revision = committed_revision;
	s.pages_retiring = pages_retiring;
	s.pages_free = allocator.get_free_page_count();
	s.largest_free_run = allocator.get_largest_free_run();
	return s;
}


// ---------------------------------------------------------------------------------------------------------------------
// Package F1: staged edits (ADR 14.5).
// ---------------------------------------------------------------------------------------------------------------------

void NodeResidency::release_edit_state() {
	for (uint32_t i = 0; i < staged_leaves.size(); ++i) {
		Node &n = nodes.write[staged_leaves[i]];
		if (n.shadow_key != 0) {
			// The shadow payload is only uploaded at commit, so a staged (cancelled) run never reached the GPU.
			allocator.release_slot(n.shadow_key);
		}
		n.shadow_key = 0;
		n.shadow_first = n.shadow_pages = 0;
		n.shadow_generation = 0;
		n.shadow_state = Node::SHADOW_NONE;
	}
	for (uint32_t i = 0; i < staged_required.size(); ++i) {
		nodes.write[staged_required[i]].edit_hold = false;
	}
	staged_leaves.clear();
	staged_splats.clear();
	staged_stale.clear();
	staged_required.clear();
	shadow_alloc_pending = false;
	edit_staged = false;
}

NodeResidency::EditResult NodeResidency::stage_edit(uint64_t p_revision, const EditedLeaf *p_leaves, uint32_t p_count, LocalVector<Ticket> &r_tickets) {
	if (nodes.is_empty() || edit_staged) {
		++totals.edits_rejected_total;
		return EDIT_BUSY;
	}
	const uint32_t node_total = nodes.size();
	auto reject = [&](EditResult p_result) {
		++totals.edits_rejected_total;
		return p_result;
	};
	if (p_leaves == nullptr || p_count == 0 || p_revision != committed_revision + 1) {
		return reject(EDIT_INVALID);
	}
	// Leaf = no node names it as a parent.
	LocalVector<uint8_t> has_child;
	has_child.resize(node_total);
	for (uint32_t i = 0; i < node_total; ++i) {
		has_child[i] = 0;
	}
	for (uint32_t i = 0; i < node_total; ++i) {
		if (nodes[i].parent < node_total) {
			has_child[nodes[i].parent] = 1;
		}
	}
	LocalVector<uint8_t> edited_flag, stale_flag;
	edited_flag.resize(node_total);
	stale_flag.resize(node_total);
	for (uint32_t i = 0; i < node_total; ++i) {
		edited_flag[i] = stale_flag[i] = 0;
	}
	for (uint32_t i = 0; i < p_count; ++i) {
		const uint32_t node = p_leaves[i].node;
		if (node >= node_total || p_leaves[i].splat_count == 0 || has_child[node] || edited_flag[node] || nodes[node].stale) {
			return reject(EDIT_INVALID);
		}
		edited_flag[node] = 1;
	}
	for (uint32_t i = 0; i < p_count; ++i) {
		for (uint32_t up = nodes[p_leaves[i].node].parent; up < node_total && !stale_flag[up]; up = nodes[up].parent) {
			stale_flag[up] = 1;
		}
	}
	// The pages the shadow runs need.
	uint32_t total_pages = 0;
	LocalVector<uint32_t> pages;
	pages.resize(p_count);
	for (uint32_t i = 0; i < p_count; ++i) {
		pages[i] = (p_leaves[i].splat_count + cfg.page_splats - 1u) / cfg.page_splats;
		total_pages += pages[i];
	}
	// Admission from free plus reclaimable pages, WITHOUT evicting: a refused edit changes nothing. The holds on the
	// replacement children are part of the decision (a held child is not reclaimable).
	// The replacement cut: every child of a stale node that is neither stale (a committed or newly stale node is only
	// ever expanded, never required to be resident) nor edited (those come with a shadow). Grouped by parent.
	LocalVector<uint32_t> required;
	LocalVector<uint32_t> stale_list;
	for (uint32_t s = 0; s < node_total; ++s) {
		if (!stale_flag[s]) { continue; }
		stale_list.push_back(s);
		for (uint32_t c = s + 1; c < node_total; ++c) {
			if (nodes[c].parent == s && !stale_flag[c] && !nodes[c].stale && !edited_flag[c]) {
				required.push_back(c);
			}
		}
	}
	uint64_t reclaimable = uint64_t(allocator.get_free_page_count()) + pages_retiring;
	for (uint32_t i = 0; i < node_total; ++i) {
		bool held = false;
		for (uint32_t r = 0; r < required.size() && !held; ++r) { held = required[r] == i; }
		// The drawn cut and its ancestors are protected every frame, but this call may come before this frame's
		// protect_drawn(): a node protected on the previous frame counts as protected here, so admission is not optimistic.
		const bool recently_protected = nodes[i].protected_frame != UINT64_MAX && nodes[i].protected_frame + 1 >= current_frame;
		if (nodes[i].state == RESIDENT && !is_protected(i) && !recently_protected && !held) {
			reclaimable += nodes[i].pages;
		}
	}
	if (reclaimable < total_pages) {
		return reject(EDIT_NO_SPACE); // Nothing was evicted, held or reserved.
	}
	// Admitted: holds first, so nothing the edit needs can be evicted from here on.
	for (uint32_t i = 0; i < required.size(); ++i) {
		nodes.write[required[i]].edit_hold = true;
	}
	for (uint32_t i = 0; i < p_count; ++i) {
		staged_leaves.push_back(p_leaves[i].node);
		staged_splats.push_back(p_leaves[i].splat_count);
	}
	staged_stale = stale_list;
	staged_required = required;
	edit_staged = true;
	staged_revision = p_revision;
	staged_frame = current_frame;
	shadow_alloc_pending = true;
	try_allocate_shadows(r_tickets); // No eviction here: advance_edit() evicts, after the frame's protection is known.
	return EDIT_OK;
}

bool NodeResidency::try_allocate_shadows(LocalVector<Ticket> &r_tickets) {
	if (!edit_staged || !shadow_alloc_pending) {
		return false;
	}
	const uint32_t count = staged_leaves.size();
	LocalVector<uint64_t> keys;
	LocalVector<uint32_t> firsts, pages;
	keys.resize(count);
	firsts.resize(count);
	pages.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		pages[i] = (staged_splats[i] + cfg.page_splats - 1u) / cfg.page_splats;
		const uint64_t key = make_alloc_key(staged_leaves[i]);
		uint32_t slot = 0;
		if (!allocator.allocate_slot(key, pages[i], slot)) {
			for (uint32_t r = 0; r < i; ++r) {
				allocator.release_slot(keys[r]);
			}
			return false;
		}
		keys[i] = key;
		firsts[i] = slot;
	}
	for (uint32_t i = 0; i < count; ++i) {
		Node &n = nodes.write[staged_leaves[i]];
		n.shadow_key = keys[i];
		n.shadow_first = firsts[i];
		n.shadow_pages = pages[i];
		n.shadow_generation = ++request_serial;
		n.shadow_state = Node::SHADOW_LOADING;
		Ticket t;
		t.node = staged_leaves[i];
		t.first_page = firsts[i];
		t.page_count = pages[i];
		t.request_generation = n.shadow_generation;
		t.world_generation = world_gen;
		t.device_generation = device_gen;
		t.shadow = true;
		r_tickets.push_back(t);
	}
	shadow_alloc_pending = false;
	return true;
}

void NodeResidency::advance_edit(LocalVector<Ticket> &r_tickets) {
	if (!edit_staged || !shadow_alloc_pending) {
		return;
	}
	if (try_allocate_shadows(r_tickets)) {
		return;
	}
	// Not enough contiguous free pages: free unprotected nodes (the drawn set and its ancestors are protected once the
	// caller ran protect_drawn for this frame; the replacement children are held). A freed run retires after the delay,
	// so the allocation is retried on a later frame.
	uint32_t total_pages = 0;
	for (uint32_t i = 0; i < staged_splats.size(); ++i) {
		total_pages += (staged_splats[i] + cfg.page_splats - 1u) / cfg.page_splats;
	}
	const uint32_t evicted = evict_for(total_pages, UINT32_MAX);
	if (try_allocate_shadows(r_tickets)) {
		return;
	}
	// Enough pages in total but no contiguous run (the retired runs are not adjacent): free the cheapest unprotected
	// neighbours of a hole, the same bounded answer request() gives to fragmentation (compaction itself is D2).
	if (evicted == 0 && pages_retiring == 0 && uint64_t(allocator.get_free_page_count()) >= total_pages) {
		for (uint32_t i = 0; i < staged_splats.size(); ++i) {
			evict_window((staged_splats[i] + cfg.page_splats - 1u) / cfg.page_splats, UINT32_MAX);
		}
	}
}

void NodeResidency::edit_prefetch(LocalVector<uint32_t> &r_demand) const {
	if (!edit_staged) {
		return;
	}
	for (uint32_t i = 0; i < staged_required.size(); ++i) {
		const uint32_t node = staged_required[i];
		if (nodes[node].state != RESIDENT) {
			r_demand.push_back(node);
		}
	}
}

bool NodeResidency::is_shadow_pending(const Ticket &p_ticket) const {
	if (!edit_staged || !p_ticket.shadow || !p_ticket.valid() || p_ticket.world_generation != world_gen || p_ticket.device_generation != device_gen ||
			p_ticket.node >= uint32_t(nodes.size())) {
		return false;
	}
	const Node &n = nodes[p_ticket.node];
	return n.shadow_state == Node::SHADOW_LOADING && n.shadow_generation == p_ticket.request_generation;
}

bool NodeResidency::finish_shadow(const Ticket &p_ticket, bool p_success) {
	if (!is_shadow_pending(p_ticket)) {
		++totals.late_dropped_total;
		return false;
	}
	nodes.write[p_ticket.node].shadow_state = p_success ? Node::SHADOW_READY : Node::SHADOW_FAILED;
	return true;
}

bool NodeResidency::edit_ready() const {
	if (!edit_staged || shadow_alloc_pending) {
		return false;
	}
	for (uint32_t i = 0; i < staged_leaves.size(); ++i) {
		if (nodes[staged_leaves[i]].shadow_state != Node::SHADOW_READY) {
			return false;
		}
	}
	for (uint32_t i = 0; i < staged_required.size(); ++i) {
		if (nodes[staged_required[i]].state != RESIDENT) {
			return false;
		}
	}
	return true;
}

bool NodeResidency::edit_blocked() const {
	if (!edit_staged) {
		return false;
	}
	for (uint32_t i = 0; i < staged_leaves.size(); ++i) {
		if (nodes[staged_leaves[i]].shadow_state == Node::SHADOW_FAILED) {
			return true;
		}
	}
	for (uint32_t i = 0; i < staged_required.size(); ++i) {
		const Node &n = nodes[staged_required[i]];
		if (n.state == FAILED && n.failures >= cfg.max_failures) {
			return true;
		}
	}
	return false;
}

bool NodeResidency::commit_edit() {
	if (!edit_ready()) {
		return false;
	}
	for (uint32_t i = 0; i < staged_leaves.size(); ++i) {
		const uint32_t index = staged_leaves[i];
		Node &n = nodes.write[index];
		if (n.state == LOADING || n.state == UPLOADING) {
			++totals.cancelled_total; // A load of the OLD content is in flight: its ticket dies below.
		}
		retire_run(n); // Uses the old page count; the old run serves already submitted frames until it retires.
		n.alloc_key = n.shadow_key;
		n.first_page = n.shadow_first;
		n.pages = n.shadow_pages;
		n.request_generation = ++request_serial; // Any ticket made for the old content is dead.
		n.state = RESIDENT;
		n.failures = 0;
		n.last_drawn_frame = current_frame;
		mask.write[index] = 1;
		n.shadow_key = 0;
		n.shadow_first = n.shadow_pages = 0;
		n.shadow_generation = 0;
		n.shadow_state = Node::SHADOW_NONE;
		for (uint32_t e = 0; e < evicted_log.size(); ++e) { // A pending clear must not zero the new metadata.
			if (evicted_log[e] == index) {
				evicted_log[e] = evicted_log[evicted_log.size() - 1];
				evicted_log.resize(evicted_log.size() - 1);
				break;
			}
		}
	}
	for (uint32_t i = 0; i < staged_stale.size(); ++i) {
		const uint32_t index = staged_stale[i];
		Node &n = nodes.write[index];
		if (n.state == LOADING || n.state == UPLOADING) {
			++totals.cancelled_total;
		}
		const bool was_resident = n.state == RESIDENT;
		n.stale = true;
		n.state = ABSENT;
		n.request_generation = 0;
		n.failures = 0;
		mask.write[index] = 0;
		retire_run(n);
		if (was_resident) {
			evicted_log.push_back(index); // The glue clears its GPU metadata.
			++totals.evicted_total;
		}
	}
	committed_revision = staged_revision;
	++totals.edits_committed_total;
	for (uint32_t i = 0; i < staged_required.size(); ++i) {
		nodes.write[staged_required[i]].edit_hold = false;
	}
	staged_leaves.clear();
	staged_splats.clear();
	staged_stale.clear();
	staged_required.clear();
	edit_staged = false;
	return true;
}

bool NodeResidency::cancel_edit() {
	if (!edit_staged) {
		return false;
	}
	release_edit_state();
	++totals.edits_cancelled_total;
	return true;
}

} // namespace gs_hlod_runtime
