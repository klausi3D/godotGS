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
	}
	return true;
}

bool NodeResidency::reset(const Vector<NodeInfo> &p_nodes, const Config &p_config, uint64_t p_world_generation, uint64_t p_device_generation) {
	nodes.clear();
	mask.clear();
	retiring.clear();
	candidates.clear();
	evicted_log.clear();
	allocator.clear();
	totals = Stats();
	pages_retiring = 0;
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
		n.pages = (info.splat_count + cfg.page_splats - 1u) / cfg.page_splats;
	}
	nodes = std::move(next);
	mask = std::move(next_mask);
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
	return n.protected_frame == current_frame || (cfg.pin_root && p_node == 0) || n.state == LOADING || n.state == UPLOADING;
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

NodeResidency::RequestResult NodeResidency::request(const uint32_t *p_demand, uint32_t p_count, LocalVector<Ticket> &r_tickets) {
	RequestResult out;
	for (uint32_t i = 0; i < p_count; ++i) {
		if (p_demand[i] < uint32_t(nodes.size())) {
			nodes.write[p_demand[i]].last_demand_frame = current_frame;
		}
	}
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
			if (n.state == LOADING || n.state == UPLOADING || n.state == RESIDENT) {
				++out.satisfied;
			} else if (n.state == FAILED && (n.failures >= cfg.max_failures || current_frame < n.retry_after_frame)) {
				blocked = true; // A child cannot arrive now, so the parent's whole transition cannot complete.
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
		auto allocate_all = [&]() {
			for (uint32_t k = 0; k < need_count; ++k) {
				Node &n = nodes.write[need[k]];
				const uint64_t key = make_alloc_key(need[k]);
				uint32_t slot = 0;
				if (!allocator.allocate_slot(key, n.pages, slot)) {
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
		if (!placed) {
			++out.skipped_groups;
			++totals.skipped_groups_total;
			continue; // Skipped, not waited for: the parent keeps being drawn.
		}
		want_group();
		for (uint32_t k = 0; k < need_count; ++k) {
			Node &n = nodes.write[need[k]];
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
	return p_ticket.valid() && p_ticket.world_generation == world_gen && p_ticket.device_generation == device_gen &&
			p_ticket.node < uint32_t(nodes.size()) && nodes[p_ticket.node].request_generation == p_ticket.request_generation &&
			nodes[p_ticket.node].state == p_expected;
}

bool NodeResidency::begin_upload(const Ticket &p_ticket) {
	if (!ticket_current(p_ticket, LOADING)) {
		++totals.late_dropped_total;
		return false;
	}
	nodes.write[p_ticket.node].state = UPLOADING;
	return true;
}

bool NodeResidency::finish_upload(const Ticket &p_ticket, bool p_success) {
	if (!ticket_current(p_ticket, UPLOADING)) {
		++totals.late_dropped_total;
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
		++totals.late_dropped_total;
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
	s.pages_retiring = pages_retiring;
	s.pages_free = allocator.get_free_page_count();
	s.largest_free_run = allocator.get_largest_free_run();
	return s;
}

} // namespace gs_hlod_runtime
