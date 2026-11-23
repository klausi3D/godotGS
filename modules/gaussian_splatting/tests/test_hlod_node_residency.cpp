extern "C" int test_hlod_node_residency_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../lod/hlod_node_residency.h"
#include "../lod/hlod_runtime_selection.h"
#include "../renderer/hlod_node_streamer.h"
#include <algorithm>
#include <vector>

// ADR 6.6 / 14.1 / 14.2, package D1: demand -> request -> resident-ready -> drawable, protection, cancellation
// and late completions. CPU only: the model owns no GPU resource, so these tests drive the same state machine
// the renderer glue drives, with the real selector (CutHistory) deciding what is drawn.
namespace TestHlodNodeResidency {
using namespace gs_hlod_runtime;
using Residency = NodeResidency;

// Complete binary tree in BFS order: node i has children 2i+1, 2i+2. Leaves are at the last level.
// Interiors are 1 page, leaves are 2 pages (mixed run sizes). Nodes sit on a line, so camera distance
// changes which parts of the tree the selector wants refined.
static void binary_tree(GaussianSplatHlodRuntimeData &r_data, uint32_t p_levels, uint32_t p_leaf_splats = 1500, uint32_t p_interior_splats = 1000) {
	const uint32_t count = (1u << p_levels) - 1u;
	r_data.nodes.resize(count);
	const uint32_t first_leaf = (1u << (p_levels - 1)) - 1u;
	const uint32_t leaves = count - first_leaf;
	for (uint32_t i = 0; i < count; ++i) {
		auto &n = r_data.nodes.write[i];
		// Height 0 = leaf. The node's x range is its span of leaves.
		uint32_t level = 0;
		while (((1u << (level + 1)) - 1u) <= i) { ++level; }
		const uint32_t height = p_levels - 1 - level;
		const uint32_t span = 1u << height;
		const uint32_t first = (i - ((1u << level) - 1u)) * span;
		n.bounds_center = Vector3((float(first) + float(span) * 0.5f) * 10.0f, 0, -10);
		n.topology.radius = float(span) * 5.0f;
		n.topology.height = height;
		n.topology.payload_count = i >= first_leaf ? p_leaf_splats : p_interior_splats;
		n.topology.geometric_error = height == 0 ? 0.0f : float(1u << height);
		n.topology.parent = i == 0 ? gs_hlod::kNoParent : (i - 1) / 2;
		if (i < first_leaf) {
			n.topology.first_child = 2 * i + 1;
			n.topology.child_count = 2;
		}
	}
	(void)leaves;
}

static CutRequest base_request() {
	CutRequest r;
	r.frustum_enabled = false;
	r.focal_pixels = 100;
	r.tau_pixels = 1;
	r.record_budget = 1.0e9;
	r.splat_budget = 1u << 30;
	return r;
}

static Residency::Config config(uint32_t p_pages, uint32_t p_retire = 2) {
	Residency::Config c;
	c.page_capacity = p_pages;
	c.retire_delay_frames = p_retire;
	c.max_requests_per_frame = 64;
	c.evictions_per_frame = 64;
	return c;
}

// One simulated renderer frame: select against the resident mask, protect, request. Tickets are returned so the
// caller decides when (and whether) their payloads "arrive".
struct Sim {
	Ref<GaussianSplatHlodRuntimeData> data;
	CutHistory history;
	Residency res;
	uint64_t frame = 0;
	std::vector<uint32_t> drawn;
	std::vector<uint32_t> demand;
	CutHistory::Result cut;
	LocalVector<Residency::Ticket> tickets;
	Residency::RequestResult request_result;
	CutRequest req = base_request();

	bool init(uint32_t p_levels, uint32_t p_pages, uint32_t p_retire = 2) {
		data.instantiate();
		binary_tree(*data.ptr(), p_levels);
		Vector<Residency::NodeInfo> infos;
		return make_node_infos(*data.ptr(), infos) && history.reserve(data->nodes.size()) &&
				res.reset(infos, config(p_pages, p_retire), 1, 1);
	}
	void step(bool p_request = true) {
		res.begin_frame(++frame);
		req.resident = res.resident_mask().ptr();
		req.resident_count = res.resident_mask().size();
		req.node_budget = data->nodes.size();
		cut = history.select(*data.ptr(), req);
		drawn.clear();
		demand.clear();
		const bool complete = cut.status == CutHistory::COMPLETE || cut.status == CutHistory::EMPTY;
		for (uint32_t i = 0; complete && i < cut.selected_count; ++i) { drawn.push_back(history.selected_node(i)); }
		for (uint32_t i = 0; i < history.demand_count(); ++i) { demand.push_back(history.demand_node(i)); }
		res.protect_drawn(drawn.data(), uint32_t(drawn.size()));
		tickets.clear();
		if (p_request) { request_result = res.request(demand.data(), uint32_t(demand.size()), tickets); }
	}
	void arrive(const Residency::Ticket &p_t) {
		if (res.begin_upload(p_t)) { res.finish_upload(p_t, true); }
	}
	void arrive_all() {
		for (uint32_t i = 0; i < tickets.size(); ++i) { arrive(tickets[i]); }
	}
};

static bool every_drawn_resident(const Sim &p_sim) {
	for (uint32_t n : p_sim.drawn) {
		if (p_sim.res.state(n) != Residency::RESIDENT) { return false; }
	}
	return true;
}
// Every leaf has exactly one selected ancestor-or-self: no hole and no overlap (no node with a drawn ancestor).
static bool exact_cover(const Sim &p_sim, uint32_t p_levels) {
	const uint32_t count = (1u << p_levels) - 1u;
	for (uint32_t leaf = (1u << (p_levels - 1)) - 1u; leaf < count; ++leaf) {
		uint32_t hits = 0;
		for (uint32_t n : p_sim.drawn) {
			for (uint32_t a = leaf;; a = (a - 1) / 2) {
				if (a == n) { ++hits; break; }
				if (a == 0) { break; }
			}
		}
		if (hits != 1) { return false; }
	}
	return true;
}


TEST_CASE("[GaussianSplatting][HLOD][Residency] a refinement waits for every child, then replaces the parent as a whole") {
	Sim sim;
	if (!sim.init(3, 64)) { FAIL("residency setup failed"); return; }
	// Frame 1: nothing resident. Loading: no cut, the root is the demand and gets requested.
	sim.step();
	CHECK(sim.cut.status == CutHistory::UNAVAILABLE);
	CHECK(sim.drawn.empty());
	CHECK(sim.demand == std::vector<uint32_t>{ 0 });
	if (sim.tickets.size() != 1) { FAIL("expected one root ticket"); return; }
	// Allocated but not resident-ready until the upload completed: still loading, nothing drawable.
	CHECK(sim.res.state(0) == Residency::LOADING);
	sim.step();
	CHECK(sim.cut.status == CutHistory::UNAVAILABLE);
	CHECK(sim.drawn.empty());
	// The root's payload arrives and is uploaded: the coarse representation is ready.
	Sim s;
	if (!s.init(3, 64)) { FAIL("residency setup failed"); return; }
	s.step();
	s.arrive_all();
	s.step();
	CHECK(s.cut.status == CutHistory::COMPLETE);
	CHECK(s.drawn == std::vector<uint32_t>{ 0 }); // READY with the coarse representation.
	CHECK(s.demand == std::vector<uint32_t>{ 1, 2 });
	if (s.tickets.size() != 2) { FAIL("expected the whole child group to be requested"); return; }
	// Only one child arrives: the cut must not move, and every drawn node is resident-ready.
	const Residency::Ticket first = s.tickets[0];
	const Residency::Ticket second = s.tickets[1];
	s.arrive(first);
	s.step();
	CHECK(s.drawn == std::vector<uint32_t>{ 0 });
	CHECK(every_drawn_resident(s));
	CHECK(s.demand == std::vector<uint32_t>{ second.node });
	// The second child arrives: the parent is replaced by both children at once.
	s.arrive(second);
	s.step();
	CHECK(s.drawn == std::vector<uint32_t>{ 1, 2 });
	CHECK(every_drawn_resident(s));
	CHECK(s.demand.size() == 4);
	for (int guard = 0; guard < 8 && s.drawn.size() < 4; ++guard) {
		s.arrive_all();
		s.step();
		CHECK(every_drawn_resident(s));
		CHECK(exact_cover(s, 3));
	}
	CHECK(s.drawn == std::vector<uint32_t>{ 3, 4, 5, 6 });
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] cancelling a request mid-upload keeps the old cut and the old run is not reusable until it retires") {
	// Capacity for exactly the root and its two interior children (1 page each); retirement takes 3 frames.
	Sim sim;
	if (!sim.init(3, 3, 3)) { FAIL("residency setup failed"); return; }
	sim.step(); // frame 1
	sim.arrive_all();
	sim.step(); // frame 2: root drawn; children requested.
	if (sim.tickets.size() != 2) { FAIL("expected two child tickets"); return; }
	const Residency::Ticket a = sim.tickets[0];
	const Residency::Ticket b = sim.tickets[1];
	CHECK(sim.res.begin_upload(a));
	CHECK(sim.res.finish_upload(a, true));
	CHECK(sim.res.begin_upload(b)); // b is UPLOADING (GPU copy issued) when it is cancelled.
	CHECK(sim.res.state(b.node) == Residency::UPLOADING);
	CHECK(sim.res.cancel(b.node));
	// The late completion of the cancelled request cannot publish.
	CHECK_FALSE(sim.res.finish_upload(b, true));
	CHECK(sim.res.state(b.node) == Residency::ABSENT);
	CHECK(sim.res.resident_mask()[b.node] == 0);
	CHECK(sim.res.stats().late_dropped_cancelled_total == 1);
	CHECK(sim.res.stats().late_dropped_total == 0);
	// The old cut is kept: the parent is still drawn, and nothing partial appears.
	sim.step(false); // frame 3
	CHECK(sim.drawn == std::vector<uint32_t>{ 0 });
	CHECK(every_drawn_resident(sim));
	// The cancelled run still counts against the budget while it retires: it cannot be reused yet.
	CHECK(sim.res.stats().pages_retiring == sim.res.pages_for(b.node));
	CHECK(sim.res.stats().pages_free == 0);
	sim.step(); // frame 4
	CHECK(sim.request_result.skipped_groups == 1);
	CHECK(sim.tickets.size() == 0);
	CHECK(sim.res.state(b.node) == Residency::ABSENT);
	// After the retirement delay the run is free again and a new request (a new generation) is accepted.
	sim.step(); // frame 5
	if (sim.tickets.size() != 1) { FAIL("expected the cancelled child to be re-requested after retirement"); return; }
	const Residency::Ticket again = sim.tickets[0];
	CHECK(again.node == b.node);
	CHECK(again.request_generation != b.request_generation);
	CHECK_FALSE(sim.res.begin_upload(b)); // The old ticket stays dead even though the node is in flight again.
	CHECK(sim.res.begin_upload(again));
	CHECK(sim.res.finish_upload(again, true));
	sim.step();
	CHECK(sim.drawn == std::vector<uint32_t>{ 1, 2 });
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a late completion cannot resurrect an unloaded world, an old device or a finished request") {
	Sim sim;
	if (!sim.init(3, 16)) { FAIL("residency setup failed"); return; }
	sim.step();
	if (sim.tickets.size() != 1) { FAIL("expected a root ticket"); return; }
	const Residency::Ticket old_world = sim.tickets[0];
	Vector<Residency::NodeInfo> infos;
	if (!make_node_infos(*sim.data.ptr(), infos)) { FAIL("node infos"); return; }
	// Unload and reload: a new world generation with the same node table. The new world requests its root
	// first, so its ticket carries the very same request generation and node as the old one: only the world
	// generation tells them apart. The old worker finishes late.
	CHECK(sim.res.reset(infos, config(16), 2, 1));
	sim.res.begin_frame(1);
	LocalVector<Residency::Ticket> tickets;
	const uint32_t root[1] = { 0 };
	sim.res.request(root, 1, tickets);
	if (tickets.size() != 1) { FAIL("expected a root ticket in the new world"); return; }
	const Residency::Ticket new_world = tickets[0];
	CHECK(new_world.request_generation == old_world.request_generation);
	CHECK(new_world.node == old_world.node);
	CHECK_FALSE(sim.res.begin_upload(old_world));
	CHECK(sim.res.state(0) == Residency::LOADING); // The new world's request is untouched.
	CHECK(sim.res.begin_upload(new_world));
	CHECK_FALSE(sim.res.finish_upload(old_world, true)); // Not the resurrection of the old world's node either.
	CHECK(sim.res.resident_mask()[0] == 0);
	// A new device generation (device lost / recreated) rejects tickets of the old device the same way.
	CHECK(sim.res.reset(infos, config(16), 2, 2));
	sim.res.begin_frame(1);
	tickets.clear();
	sim.res.request(root, 1, tickets);
	if (tickets.size() != 1) { FAIL("expected a root ticket on the new device"); return; }
	const Residency::Ticket new_device = tickets[0];
	CHECK(new_device.request_generation == new_world.request_generation);
	CHECK_FALSE(sim.res.begin_upload(new_world)); // Same world, node and request generation: only the device differs.
	CHECK(sim.res.state(0) == Residency::LOADING);
	CHECK(sim.res.resident_mask()[0] == 0);
	// A request completes once: a duplicate completion is not a second publication.
	const uint64_t dropped_before = sim.res.stats().late_dropped_total;
	CHECK(sim.res.begin_upload(new_device));
	CHECK(sim.res.finish_upload(new_device, true));
	CHECK(sim.res.stats().late_dropped_total == dropped_before);
	CHECK_FALSE(sim.res.finish_upload(new_device, true));
	CHECK(sim.res.stats().late_dropped_total == dropped_before + 1);
	CHECK(sim.res.stats().completed_total == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a cancelled request completing late is harmless, a wanted completion that is discarded is real loss") {
	// Case 1: cancelled, then the worker completes late. Only the harmless counter moves.
	{
		Sim sim;
		if (!sim.init(3, 16)) { FAIL("residency setup failed"); return; }
		sim.step();
		if (sim.tickets.size() != 1) { FAIL("expected a root ticket"); return; }
		const Residency::Ticket t = sim.tickets[0];
		CHECK(sim.res.cancel(t.node));
		CHECK_FALSE(sim.res.begin_upload(t)); // The late "payload ready" of a cancelled request.
		CHECK(sim.res.stats().late_dropped_cancelled_total == 1);
		CHECK(sim.res.stats().late_dropped_total == 0);
		// A second delivery of the same cancelled ticket is a duplicate, not another cancel: real loss.
		CHECK_FALSE(sim.res.fail_load(t));
		CHECK(sim.res.stats().late_dropped_cancelled_total == 1);
		CHECK(sim.res.stats().late_dropped_total == 1);
		CHECK(sim.res.stats().cancelled_total == 1);
		CHECK(sim.res.stats().completed_total == 0);
	}
	// Case 2: the request is wanted (never cancelled) but its completion is discarded. Only the loss counter moves.
	{
		Sim sim;
		if (!sim.init(3, 16)) { FAIL("residency setup failed"); return; }
		sim.step();
		if (sim.tickets.size() != 1) { FAIL("expected a root ticket"); return; }
		const Residency::Ticket t = sim.tickets[0];
		// An upload finished out of order (the node is still LOADING) is a discarded wanted completion.
		CHECK_FALSE(sim.res.finish_upload(t, true));
		CHECK(sim.res.stats().late_dropped_total == 1);
		CHECK(sim.res.stats().late_dropped_cancelled_total == 0);
		CHECK(sim.res.state(t.node) == Residency::LOADING); // Untouched: the request is still wanted.
		// A ticket of an older world is discarded the same way, even if its node was cancelled in the new world.
		Vector<Residency::NodeInfo> infos;
		if (!make_node_infos(*sim.data.ptr(), infos)) { FAIL("node infos"); return; }
		CHECK(sim.res.reset(infos, config(16), 2, 1));
		sim.res.begin_frame(1);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t root[1] = { 0 };
		sim.res.request(root, 1, tickets);
		if (tickets.size() != 1) { FAIL("expected a root ticket in the new world"); return; }
		CHECK(sim.res.cancel(0)); // Same node and same request generation as the old world's ticket.
		CHECK_FALSE(sim.res.begin_upload(t));
		CHECK(sim.res.stats().late_dropped_total == 1);
		CHECK(sim.res.stats().late_dropped_cancelled_total == 0);
		CHECK_FALSE(sim.res.begin_upload(tickets[0])); // The new world's own cancelled ticket is harmless.
		CHECK(sim.res.stats().late_dropped_total == 1);
		CHECK(sim.res.stats().late_dropped_cancelled_total == 1);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] the cancelled window is bounded: a ticket older than the window counts as real loss") {
	Sim sim;
	if (!sim.init(3, 16, 0)) { FAIL("residency setup failed"); return; }
	// Request and cancel the root over and over: each cycle kills one more request generation.
	Residency::Ticket first;
	Residency::Ticket last;
	const uint32_t cycles = 300; // More than the 256-entry window.
	for (uint32_t i = 0; i < cycles; ++i) {
		sim.res.begin_frame(i + 1);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t root[1] = { 0 };
		sim.res.request(root, 1, tickets);
		if (tickets.size() != 1) { FAIL("expected a root ticket in cycle ", i); return; }
		if (i == 0) { first = tickets[0]; }
		last = tickets[0];
		if (!sim.res.cancel(0)) { FAIL("cancel failed in cycle ", i); return; }
	}
	CHECK(sim.res.stats().cancelled_total == cycles);
	// The newest cancel is inside the window: harmless. The oldest was pushed out: counted as real loss.
	CHECK_FALSE(sim.res.begin_upload(last));
	CHECK(sim.res.stats().late_dropped_cancelled_total == 1);
	CHECK(sim.res.stats().late_dropped_total == 0);
	CHECK_FALSE(sim.res.begin_upload(first));
	CHECK(sim.res.stats().late_dropped_cancelled_total == 1);
	CHECK(sim.res.stats().late_dropped_total == 1);
}

// 15-node tree with one page per node, so eviction order and protection are the only things under test.
static bool one_page_tree(Sim &r_sim, uint32_t p_pages, uint32_t p_retire = 0) {
	r_sim.data.instantiate();
	binary_tree(*r_sim.data.ptr(), 4, 1000, 1000);
	Vector<Residency::NodeInfo> infos;
	return make_node_infos(*r_sim.data.ptr(), infos) && r_sim.history.reserve(r_sim.data->nodes.size()) &&
			r_sim.res.reset(infos, config(p_pages, p_retire), 1, 1);
}
static bool make_resident_at(Residency &r_res, uint32_t p_node, uint64_t p_frame) {
	LocalVector<Residency::Ticket> t;
	r_res.begin_frame(p_frame);
	const uint32_t demand[1] = { p_node };
	r_res.request(demand, 1, t);
	return t.size() == 1 && r_res.begin_upload(t[0]) && r_res.finish_upload(t[0], true);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] the protected closure prevents eviction of drawn nodes and their ancestors") {
	Sim sim;
	if (!one_page_tree(sim, 6)) { FAIL("residency setup failed"); return; }
	// Resident at the same frame, so the drawn stamp cannot order them: the chain 7 -> 3 -> 1 -> 0 plus 8, 9.
	for (uint32_t n : { 0u, 1u, 3u, 7u, 8u, 9u }) {
		if (!make_resident_at(sim.res, n, 5)) { FAIL("could not make node resident"); return; }
	}
	CHECK(sim.res.stats().pages_free == 0);
	// Frame 20: leaf 7 is drawn this frame. Protect it and demand a 2-page group (children of node 6).
	sim.res.begin_frame(20);
	const uint32_t drawn[1] = { 7 };
	sim.res.protect_drawn(drawn, 1);
	CHECK(sim.res.is_protected(3));
	CHECK(sim.res.is_protected(1));
	CHECK_FALSE(sim.res.is_protected(8));
	CHECK_FALSE(sim.res.evict(7)); // Direct eviction of a protected node is refused.
	CHECK_FALSE(sim.res.evict(3));
	LocalVector<Residency::Ticket> tickets;
	const uint32_t demand[2] = { 13, 14 };
	const auto result = sim.res.request(demand, 2, tickets);
	// Two pages were needed and only the two unprotected nodes could pay: the drawn chain survived.
	CHECK(result.evicted == 2);
	CHECK(tickets.size() == 2);
	CHECK(sim.res.state(8) == Residency::ABSENT);
	CHECK(sim.res.state(9) == Residency::ABSENT);
	for (uint32_t n : { 0u, 1u, 3u, 7u }) { CHECK(sim.res.state(n) == Residency::RESIDENT); }
	// In-flight work is protected too: the freshly requested nodes cannot be evicted while LOADING.
	CHECK_FALSE(sim.res.evict(13));
	CHECK(sim.res.is_protected(13));
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] pressure telemetry distinguishes measured zero and fullness while fragmented pages remain usable") {
	{
		Sim sim;
		CHECK_FALSE(sim.res.stats().pressure_measured);
		if (!one_page_tree(sim, 16)) { FAIL("residency setup failed"); return; }
		if (!make_resident_at(sim.res, 0, 1)) { FAIL("could not make root resident"); return; }
		sim.res.begin_frame(2);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t demand[2] = { 1, 2 };
		const auto result = sim.res.request(demand, 2, tickets);
		CHECK(result.requested == 2);
		const auto stats = sim.res.stats();
		CHECK(stats.pressure_measured);
		CHECK(stats.allocation_refused_total == 0);
		CHECK(stats.allocation_no_contiguous_run_total == 0);
		CHECK(stats.allocation_protected_capacity_total == 0);
		CHECK(stats.pages_free == 13);
		CHECK(stats.largest_free_run == 13);
	}
	{
		Sim sim;
		if (!one_page_tree(sim, 4)) { FAIL("residency setup failed"); return; }
		for (uint32_t n : { 0u, 1u, 3u, 7u }) {
			if (!make_resident_at(sim.res, n, 5)) { FAIL("could not fill the atlas"); return; }
		}
		sim.res.begin_frame(20);
		const uint32_t drawn[1] = { 7 };
		sim.res.protect_drawn(drawn, 1);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t demand[2] = { 13, 14 };
		const auto result = sim.res.request(demand, 2, tickets);
		CHECK(result.skipped_groups == 1);
		CHECK(result.evicted == 0);
		CHECK(tickets.size() == 0);
		const auto stats = sim.res.stats();
		CHECK(stats.pressure_measured);
		CHECK(stats.pages_free == 0);
		CHECK(stats.largest_free_run == 0);
		CHECK(stats.allocation_refused_total == 1);
		CHECK(stats.allocation_protected_capacity_total == 1);
		CHECK(stats.allocation_no_contiguous_run_total == 0);
		CHECK(stats.skipped_groups_total == stats.allocation_refused_total);
	}
	{
		// Root at page 0, node 2 at pages 1-2, node 4 at page 3. After evicting 2 there
		// are three free pages, split by protected node 4. Node 3 needs three pages.
		Residency res;
		Vector<Residency::NodeInfo> infos;
		infos.resize(5);
		const uint32_t splats[5] = { 1000, 1000, 1500, 2500, 1000 };
		const uint32_t parents[5] = { UINT32_MAX, 0, 0, 1, 0 };
		for (uint32_t i = 0; i < 5; ++i) {
			infos.write[i].splat_count = splats[i];
			infos.write[i].parent = parents[i];
		}
		if (!res.reset(infos, config(5, 0), 1, 1)) { FAIL("residency setup failed"); return; }
		for (uint32_t n : { 0u, 2u, 4u }) {
			if (!make_resident_at(res, n, n + 1u)) { FAIL("could not arrange atlas runs"); return; }
		}
		res.begin_frame(10);
		res.withdraw_transitions(); // Fixture loads are complete; these runs must be evictable, not admitted siblings.
		if (!res.evict(2)) { FAIL("could not create the fragmented free runs"); return; }
		const uint32_t drawn[1] = { 4 };
		res.protect_drawn(drawn, 1);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t demand[1] = { 3 };
		const auto result = res.request(demand, 1, tickets);
		CHECK(result.skipped_groups == 0);
		CHECK(result.fragmented_groups == 0);
		CHECK(result.evicted == 0);
		CHECK(tickets.size() == 1);
		const auto stats = res.stats();
		CHECK(stats.pressure_measured);
		CHECK(stats.pages_free == 0);
		CHECK(stats.largest_free_run == 0);
		CHECK(stats.allocation_refused_total == 0);
		CHECK(stats.allocation_no_contiguous_run_total == 0);
		CHECK(stats.allocation_protected_capacity_total == 0);
		CHECK(stats.allocation_over_page_budget_total == 0);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] eviction observations include future transition siblings without protecting them") {
	Sim sim;
	if (!one_page_tree(sim, 6)) { FAIL("residency setup failed"); return; }
	for (uint32_t n : { 0u, 1u, 3u, 7u, 8u, 9u }) {
		if (!make_resident_at(sim.res, n, 5)) { FAIL("could not make node resident"); return; }
	}
	sim.res.begin_frame(20);
	const uint32_t drawn[1] = { 7 };
	sim.res.protect_drawn(drawn, 1);
	CHECK_FALSE(sim.res.evict(7));
	CHECK_FALSE(sim.res.evict(3));
	LocalVector<Residency::Ticket> tickets;
	// Higher-priority group 13/14 evicts 8 and 9. Missing child 10 is demanded later, so
	// its resident sibling 9 is a transition dependency even before that group is visited.
	const uint32_t demand[3] = { 13, 14, 10 };
	const auto result = sim.res.request(demand, 3, tickets);
	CHECK(result.evicted == 2);
	CHECK(sim.res.state(8) == Residency::ABSENT);
	CHECK(sim.res.state(9) == Residency::ABSENT);
	for (uint32_t n : { 0u, 1u, 3u, 7u }) { CHECK(sim.res.state(n) == Residency::RESIDENT); }
	const auto stats = sim.res.stats();
	CHECK(stats.evicted_total == 2);
	CHECK(stats.evicted_in_cut_total == 0);
	CHECK(stats.evicted_in_transition_total == 1);
	CHECK(stats.evicted_neither_total == 1);
	CHECK(stats.eviction_events_dropped_total == 0);
	CHECK(sim.res.eviction_event_count() == 2);
	Residency::EvictionEvent first, second;
	if (!sim.res.eviction_event(0, first) || !sim.res.eviction_event(1, second)) {
		FAIL("two actual eviction events were not retained"); return;
	}
	CHECK(first.sequence == 1);
	CHECK(first.frame == 20);
	CHECK(first.node == 8);
	CHECK(first.classification == Residency::EVICTION_NEITHER);
	CHECK(second.sequence == 2);
	CHECK(second.frame == 20);
	CHECK(second.node == 9);
	CHECK(second.classification == Residency::EVICTION_IN_TRANSITION);
	CHECK_FALSE(sim.res.eviction_event(2, second));
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] eviction report ring retains the newest events and counts overwritten observations") {
	Sim sim;
	if (!one_page_tree(sim, 2)) { FAIL("residency setup failed"); return; }
	if (!make_resident_at(sim.res, 0, 1)) { FAIL("could not make root resident"); return; }
	const uint32_t count = Residency::EVICTION_EVENT_CAPACITY + 3u;
	for (uint32_t i = 0; i < count; ++i) {
		if (!make_resident_at(sim.res, 7, 2u + uint64_t(i) * 2u)) {
			FAIL("could not reload the next real eviction victim"); return;
		}
		sim.res.begin_frame(3u + uint64_t(i) * 2u);
		sim.res.withdraw_transitions(); // Finish the fixture's reload ownership before exercising a real eviction.
		if (!sim.res.evict(7)) { FAIL("expected a real unprotected eviction"); return; }
	}
	CHECK(sim.res.eviction_event_count() == Residency::EVICTION_EVENT_CAPACITY);
	const auto stats = sim.res.stats();
	CHECK(stats.evicted_total == count);
	CHECK(stats.evicted_neither_total == count);
	CHECK(stats.evicted_in_cut_total == 0);
	CHECK(stats.eviction_events_dropped_total == 3);
	Residency::EvictionEvent first, last;
	if (!sim.res.eviction_event(0, first) || !sim.res.eviction_event(Residency::EVICTION_EVENT_CAPACITY - 1u, last)) {
		FAIL("bounded ring lost a retained endpoint"); return;
	}
	CHECK(first.sequence == 4);
	CHECK(first.frame == 9);
	CHECK(last.sequence == count);
	CHECK(last.frame == 3u + uint64_t(count - 1u) * 2u);
	CHECK(last.node == 7);
	CHECK(last.classification == Residency::EVICTION_NEITHER);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] eviction order is least recently drawn, then finest level, then index") {
	Sim sim;
	if (!one_page_tree(sim, 6)) { FAIL("residency setup failed"); return; }
	// Last drawn frame (= completion frame) of each resident node: 7@5 8@5 3@5 (leaf, leaf, interior), 10@7, 9@9.
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 3, 5) || !make_resident_at(sim.res, 7, 5) ||
			!make_resident_at(sim.res, 8, 5) || !make_resident_at(sim.res, 10, 7) || !make_resident_at(sim.res, 9, 9)) {
		FAIL("could not make nodes resident");
		return;
	}
	sim.res.begin_frame(30);
	LocalVector<Residency::Ticket> tickets;
	const uint32_t first[2] = { 13, 14 };
	auto result = sim.res.request(first, 2, tickets);
	CHECK(result.evicted == 2);
	// Frame 5 holds 7, 8 (leaves) and 3 (interior): the leaves go first, ordered by index.
	CHECK(sim.res.state(7) == Residency::ABSENT);
	CHECK(sim.res.state(8) == Residency::ABSENT);
	CHECK(sim.res.state(3) == Residency::RESIDENT);
	sim.res.begin_frame(31);
	const uint32_t second[2] = { 11, 12 };
	result = sim.res.request(second, 2, tickets);
	CHECK(result.evicted == 2);
	// Then the remaining frame-5 node (3), then frame 7 (10); frame 9 (9) outlives both.
	CHECK(sim.res.state(3) == Residency::ABSENT);
	CHECK(sim.res.state(10) == Residency::ABSENT);
	CHECK(sim.res.state(9) == Residency::RESIDENT);
	CHECK(sim.res.state(0) == Residency::RESIDENT); // The pinned root is never a victim.
}

// Stroboscopic flicker on the reef world (forced streaming, slow pan): node 5, a child of the refined root, was out of
// view, so it was neither drawn nor an ancestor of a drawn node. LRU evicted it before nodes drawn more recently. One
// frame later the pan brought it back into the frustum: the root's visible children were no longer all resident, so the
// root replaced its whole subtree (60 splats for the whole world) until node 5 reloaded two frames later.
// A resident child of a refined node is part of that refinement: it must outlive nodes nothing depends on.
TEST_CASE("[GaussianSplatting][HLOD][Residency] a resident child of a refined node outlives unrelated nodes while it is out of view") {
	Sim sim;
	if (!one_page_tree(sim, 7)) { FAIL("residency setup failed"); return; }
	// 0..4 resident since frame 1; 11 and 12 (under node 5, which nothing refines) were drawn later, at frame 8.
	for (uint32_t n : { 0u, 1u, 2u, 3u, 4u }) {
		if (!make_resident_at(sim.res, n, 1)) { FAIL("could not make node resident"); return; }
	}
	for (uint32_t n : { 11u, 12u }) {
		if (!make_resident_at(sim.res, n, 8)) { FAIL("could not make node resident"); return; }
	}
	CHECK(sim.res.stats().pages_free == 0);
	// Frame 20: node 2 is out of view, the drawn cut under the refined root is {3, 4}.
	sim.res.begin_frame(20);
	const uint32_t drawn[2] = { 3, 4 };
	sim.res.protect_drawn(drawn, 2);
	CHECK_FALSE(sim.res.is_protected(2)); // Not hard protection: it stays a victim of last resort.
	LocalVector<Residency::Ticket> tickets;
	const uint32_t demand[2] = { 7, 8 };
	const auto result = sim.res.request(demand, 2, tickets);
	CHECK(result.evicted == 2);
	CHECK(tickets.size() == 2);
	CHECK(sim.res.state(11) == Residency::ABSENT);
	CHECK(sim.res.state(12) == Residency::ABSENT);
	CHECK(sim.res.state(2) == Residency::RESIDENT);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] an out-of-view child re-entering the frustum does not collapse its refined parent") {
	Sim sim;
	if (!one_page_tree(sim, 7)) { FAIL("residency setup failed"); return; }
	for (uint32_t n : { 0u, 1u, 2u, 3u, 4u }) {
		if (!make_resident_at(sim.res, n, 1)) { FAIL("could not make node resident"); return; }
	}
	for (uint32_t n : { 11u, 12u }) {
		if (!make_resident_at(sim.res, n, 8)) { FAIL("could not make node resident"); return; }
	}
	// Frame 20: the view ends at x = 35, so node 2 (x 40..80) is outside it while node 1's subtree is inside.
	sim.frame = 19;
	sim.req.frustum_enabled = true;
	sim.req.frustum[0] = Plane(Vector3(1, 0, 0), 35);
	for (int p = 1; p < 6; ++p) { sim.req.frustum[p] = Plane(Vector3(0, 1, 0), 1.0e6); }
	sim.step(); // Selects, protects the drawn cut and requests its demand (which needs eviction).
	CHECK(sim.cut.status == CutHistory::COMPLETE);
	CHECK(sim.drawn == std::vector<uint32_t>{ 3, 4 });
	CHECK(sim.res.state(2) == Residency::RESIDENT);
	sim.arrive_all();
	// Frame 21: the camera turns, node 2 is visible again. The root stays refined; node 2 serves its region.
	sim.req.frustum_enabled = false;
	sim.step(false);
	CHECK(sim.cut.status == CutHistory::COMPLETE);
	CHECK(sim.drawn != std::vector<uint32_t>{ 0 });
	CHECK(std::find(sim.drawn.begin(), sim.drawn.end(), 2u) != sim.drawn.end());
	CHECK(every_drawn_resident(sim));
	CHECK(exact_cover(sim, 4));
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a transition is admitted whole or not at all") {
	Sim sim;
	if (!one_page_tree(sim, 3)) { FAIL("residency setup failed"); return; }
	// Root (1 page) resident; the root's two children need 2 pages and exactly 2 are free: admitted together.
	if (!make_resident_at(sim.res, 0, 1)) { FAIL("could not make root resident"); return; }
	LocalVector<Residency::Ticket> tickets;
	sim.res.begin_frame(2);
	const uint32_t group[2] = { 1, 2 };
	auto result = sim.res.request(group, 2, tickets);
	CHECK(result.requested == 2);
	CHECK(sim.res.stats().pages_in_flight == 2);
	// The next group (children of node 1) needs 2 pages and none are free: nothing is half allocated.
	sim.res.begin_frame(3);
	LocalVector<Residency::Ticket> second;
	const uint32_t next[2] = { 3, 4 };
	result = sim.res.request(next, 2, second);
	CHECK(result.requested == 0);
	CHECK(result.skipped_groups == 1);
	CHECK(second.size() == 0);
	CHECK(sim.res.stats().pages_in_flight == 2); // Still only the first group.
	CHECK(sim.res.state(3) == Residency::ABSENT);
	CHECK(sim.res.state(4) == Residency::ABSENT);
	// One page free is not enough for two: still all or none. (Zero retire delay: the cancelled run is free now.)
	CHECK(sim.res.cancel(2));
	sim.res.begin_frame(4);
	result = sim.res.request(next, 2, second);
	CHECK(result.requested == 0);
	CHECK(sim.res.stats().pages_in_flight == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] loading, ready and failed are reported separately and a failed child blocks its parent") {
	Sim sim;
	if (!sim.init(3, 32)) { FAIL("residency setup failed"); return; }
	sim.step();
	sim.arrive_all();
	sim.step(); // Children 1, 2 requested.
	if (sim.tickets.size() != 2) { FAIL("expected two child tickets"); return; }
	const Residency::Ticket ok = sim.tickets[0];
	const Residency::Ticket bad = sim.tickets[1];
	sim.arrive(ok);
	CHECK(sim.res.fail_load(bad)); // The payload could not be read.
	auto stats = sim.res.stats();
	CHECK(stats.resident == 2); // root + child
	CHECK(stats.failed == 1);
	CHECK(stats.loading == 0);
	CHECK(stats.permanently_failed == 0);
	CHECK(sim.res.state(bad.node) == Residency::FAILED);
	// A failed child never becomes drawable: the parent keeps being drawn, and no retry happens before the delay.
	sim.step();
	CHECK(sim.drawn == std::vector<uint32_t>{ 0 });
	CHECK(sim.tickets.size() == 0);
	// After the retry delay the child is requested again (a new generation).
	for (int i = 0; i < 12 && sim.tickets.size() == 0; ++i) { sim.step(); }
	if (sim.tickets.size() != 1) { FAIL("expected a retry"); return; }
	CHECK(sim.tickets[0].node == bad.node);
	CHECK(sim.tickets[0].request_generation != bad.request_generation);
	// Failing every attempt makes the failure permanent: reported as failed, never re-requested.
	CHECK(sim.res.fail_load(sim.tickets[0]));
	sim.tickets.clear();
	for (int attempt = 0; attempt < 8 && sim.res.stats().permanently_failed == 0; ++attempt) {
		for (int i = 0; i < 80 && sim.tickets.size() == 0; ++i) { sim.step(); }
		for (uint32_t t = 0; t < sim.tickets.size(); ++t) { sim.res.fail_load(sim.tickets[t]); }
		sim.tickets.clear();
	}
	CHECK(sim.res.stats().permanently_failed == 1);
	CHECK(sim.res.stats().failed == 1);
	CHECK_FALSE(sim.res.root_permanently_failed());
	for (int i = 0; i < 60; ++i) {
		sim.step();
		CHECK(sim.tickets.size() == 0);
	}
	CHECK(sim.drawn == std::vector<uint32_t>{ 0 });
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] retry cooldown preserves already admitted sibling work until its own completion (#1483 B)") {
	Sim sim;
	if (!one_page_tree(sim, 4) || !make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 1, 2)) {
		FAIL("could not prepare the resident parent and its missing children"); return;
	}
	sim.res.begin_frame(10);
	const uint32_t demand[2] = { 3, 4 };
	LocalVector<Residency::Ticket> admitted;
	sim.res.request(demand, 2, admitted);
	if (admitted.size() != 2) { FAIL("both sibling tickets must be admitted before one fails"); return; }
	const auto failed = admitted[0], sibling = admitted[1];
	bool uploading = false;
	SUBCASE("sibling still waits for its serialized read") {}
	SUBCASE("sibling has already begun upload") {
		uploading = true;
		if (!sim.res.begin_upload(sibling)) { FAIL("could not begin the sibling upload"); return; }
	}
	if (!sim.res.fail_load(failed)) { FAIL("could not fail the first admitted child"); return; }
	// Repeated polls in the same logical frame cannot retry the failed child or cancel its admitted peer.
	for (uint32_t poll = 0; poll < 3; ++poll) {
		LocalVector<Residency::Ticket> again;
		const auto blocked = sim.res.request(demand, 2, again);
		CHECK(blocked.blocked_groups == 1);
		CHECK(again.is_empty());
		CHECK(sim.res.state(sibling.node) == (uploading ? Residency::UPLOADING : Residency::LOADING));
		CHECK(sim.res.stats().cancelled_total == 0);
		CHECK(sim.res.stats().abandoned_transitions_total == 0);
		CHECK_FALSE(sim.res.is_protected(1)); // Cooldown releases the parent's soft lease.
	}
	if ((!uploading && !sim.res.begin_upload(sibling)) || !sim.res.finish_upload(sibling, true)) {
		FAIL("retry cooldown invalidated a sibling's original ticket"); return;
	}
	sim.res.begin_frame(10 + sim.res.config().retry_delay_frames);
	LocalVector<Residency::Ticket> retried;
	sim.res.request(demand, 2, retried);
	if (retried.size() != 1) { FAIL("only the failed child must retry after cooldown"); return; }
	CHECK(retried[0].node == failed.node);
	CHECK(retried[0].request_generation != failed.request_generation);
	CHECK(sim.res.state(sibling.node) == Residency::RESIDENT);
	CHECK(sim.res.stats().failed_total == 1);
}

// Synchronous model-only polling: these bounds count frames, never worker progress.
static bool init_failure_root(Residency &r_res, const Residency::Config &p_config) {
	Vector<Residency::NodeInfo> infos;
	infos.resize(1);
	infos.write[0].splat_count = 1;
	if (!r_res.reset(infos, p_config, 1, 1)) {
		FAIL("failure-policy root setup failed");
		return false;
	}
	return true;
}

static bool request_failure_root(Residency &r_res, uint64_t &r_frame, Residency::Ticket &r_ticket, uint32_t p_max_gap) {
	const uint32_t root = 0;
	for (uint32_t gap = 0; gap < p_max_gap; ++gap) {
		r_res.begin_frame(++r_frame);
		LocalVector<Residency::Ticket> tickets;
		r_res.request(&root, 1, tickets);
		if (!tickets.is_empty()) {
			if (tickets.size() != 1) { FAIL("expected exactly one root request"); return false; }
			r_ticket = tickets[0];
			return true;
		}
	}
	FAIL("root was not re-requested within ", p_max_gap, " frames");
	return false;
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] load error policy retries only busy, OOM, unavailable and cancelled reads") {
	// Enumerate the Error domain so file, corruption, validation, I/O and unknown
	// policy codes (including ERR_BUG, ERR_LINK_FAILED and FAILED) all fail closed.
	for (int value = OK; value < ERR_MAX; ++value) {
		const Error error = static_cast<Error>(value);
		const FailureClass expected = error == ERR_BUSY || error == ERR_OUT_OF_MEMORY || error == ERR_UNAVAILABLE || error == ERR_SKIP ?
				FAILURE_RETRY_LATER : FAILURE_PERMANENT;
		INFO("Error code: ", value);
		CHECK(classify_load_error(error) == expected);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] transient load failures exceed max failures and still become resident") {
	const Error errors[] = { ERR_BUSY, ERR_OUT_OF_MEMORY, ERR_UNAVAILABLE, ERR_SKIP };
	for (Error error : errors) {
		INFO("transient Error code: ", int(error));
		Residency res;
		auto cfg = config(1, 0);
		cfg.pin_root = false; // Re-request after success to check that the backoff resets.
		if (!init_failure_root(res, cfg)) { return; }
		uint64_t frame = 0;
		Residency::Ticket ticket;
		if (!request_failure_root(res, frame, ticket, 1)) { return; }
		uint64_t first_gap = 0;
		for (uint32_t attempt = 1; attempt <= 6; ++attempt) {
			if (!res.fail_load(ticket, classify_load_error(error))) { FAIL("current root failure was rejected"); return; }
			const auto stats = res.stats();
			CHECK(res.state(0) == Residency::FAILED);
			CHECK(stats.failed_total == attempt);
			CHECK(stats.failed_retry_later_total == attempt);
			CHECK(stats.failed_permanent_total == 0);
			CHECK(stats.failed_permanent_total + stats.failed_retry_later_total == stats.failed_total);
			CHECK(stats.permanently_failed == 0);
			CHECK_FALSE(res.root_permanently_failed());
			CHECK(stats.retry_later_waiting == 1);
			LocalVector<Residency::Ticket> same_frame;
			const uint32_t root = 0;
			res.request(&root, 1, same_frame);
			CHECK(same_frame.is_empty());
			const uint64_t failed_frame = frame;
			if (!request_failure_root(res, frame, ticket, cfg.retry_later_max_delay_frames)) { return; }
			if (attempt == 1) { first_gap = frame - failed_frame; }
			CHECK(res.stats().retry_later_waiting == 0);
		}
		if (!res.begin_upload(ticket) || !res.finish_upload(ticket, true)) { FAIL("retried root did not finish uploading"); return; }
		CHECK(res.state(0) == Residency::RESIDENT);
		CHECK(res.stats().retry_later_waiting == 0);
		CHECK(res.stats().permanently_failed == 0);
		CHECK(res.stats().completed_total == 1);
		res.begin_frame(++frame);
		// This model has no selector to withdraw the completed transition's ownership.
		// End that fixture ownership so the existing eviction/reload checks exercise an evictable root.
		res.withdraw_transitions();
		if (!res.evict(0) || !request_failure_root(res, frame, ticket, 1)) { FAIL("successful root could not be loaded again"); return; }
		if (!res.fail_load(ticket, FAILURE_RETRY_LATER)) { FAIL("post-success transient failure was rejected"); return; }
		const uint64_t failed_frame = frame;
		if (!request_failure_root(res, frame, ticket, cfg.retry_later_max_delay_frames)) { return; }
		CHECK(frame - failed_frame == first_gap);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] permanent load failures retain the exact attempt limit and linear delay") {
	const bool explicit_classes[] = { false, true };
	for (bool explicit_class : explicit_classes) {
		INFO("explicit permanent class: ", explicit_class);
		Residency res;
		const auto cfg = config(1, 0);
		if (!init_failure_root(res, cfg)) { return; }
		uint64_t frame = 0;
		Residency::Ticket ticket;
		if (!request_failure_root(res, frame, ticket, 1)) { return; }
		for (uint32_t attempt = 1; attempt <= cfg.max_failures; ++attempt) {
			const bool failed = explicit_class ? res.fail_load(ticket, FAILURE_PERMANENT) : res.fail_load(ticket);
			if (!failed) { FAIL("current permanent failure was rejected"); return; }
			const auto stats = res.stats();
			CHECK(stats.failed_total == attempt);
			CHECK(stats.failed_permanent_total == attempt);
			CHECK(stats.failed_retry_later_total == 0);
			CHECK(stats.retry_later_waiting == 0);
			CHECK(stats.permanently_failed == (attempt == cfg.max_failures ? 1 : 0));
			CHECK(res.root_permanently_failed() == (attempt == cfg.max_failures));
			if (attempt < cfg.max_failures) {
				const uint64_t failed_frame = frame;
				const uint32_t expected_gap = cfg.retry_delay_frames * attempt;
				if (!request_failure_root(res, frame, ticket, expected_gap)) { return; }
				CHECK(frame - failed_frame == expected_gap);
			}
		}
		res.begin_frame(frame + 10000);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t root = 0;
		res.request(&root, 1, tickets);
		CHECK(tickets.is_empty());
		// A duplicate completion must not inflate either class total.
		CHECK_FALSE(res.fail_load(ticket, FAILURE_RETRY_LATER));
		CHECK(res.stats().failed_permanent_total == cfg.max_failures);
		CHECK(res.stats().failed_retry_later_total == 0);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] retry later backoff is deterministic monotone bounded and never spins") {
	const uint32_t base_delays[] = { 0, 3, UINT32_MAX };
	for (uint32_t base_delay : base_delays) {
		INFO("base retry delay: ", base_delay);
		auto cfg = config(1, 0);
		cfg.retry_delay_frames = base_delay;
		cfg.retry_later_max_delay_frames = 7;
		cfg.max_failures = 100; // Isolate delay bounds from the permanent-attempt limit.
		Residency first, second;
		if (!init_failure_root(first, cfg) || !init_failure_root(second, cfg)) { return; }
		uint64_t first_frame = 0, second_frame = 0, previous_gap = 0;
		Residency::Ticket first_ticket, second_ticket;
		if (!request_failure_root(first, first_frame, first_ticket, 1) ||
				!request_failure_root(second, second_frame, second_ticket, 1)) { return; }
		for (uint32_t attempt = 0; attempt < 40; ++attempt) {
			if (!first.fail_load(first_ticket, FAILURE_RETRY_LATER) || !second.fail_load(second_ticket, FAILURE_RETRY_LATER)) {
				FAIL("current transient failure was rejected"); return;
			}
			LocalVector<Residency::Ticket> same_frame;
			const uint32_t root = 0;
			first.request(&root, 1, same_frame);
			CHECK(same_frame.is_empty());
			const uint64_t failed_frame = first_frame;
			if (!request_failure_root(first, first_frame, first_ticket, cfg.retry_later_max_delay_frames) ||
					!request_failure_root(second, second_frame, second_ticket, cfg.retry_later_max_delay_frames)) { return; }
			const uint64_t gap = first_frame - failed_frame;
			CHECK(gap >= 1);
			CHECK(gap <= cfg.retry_later_max_delay_frames);
			CHECK(gap >= previous_gap);
			CHECK(first_frame == second_frame);
			previous_gap = gap;
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a root that can never load is a failure, not loading") {
	Sim sim;
	if (!sim.init(3, 32)) { FAIL("residency setup failed"); return; }
	for (int i = 0; i < 200 && !sim.res.root_permanently_failed(); ++i) {
		sim.step();
		for (uint32_t t = 0; t < sim.tickets.size(); ++t) { sim.res.fail_load(sim.tickets[t]); }
	}
	CHECK(sim.res.root_permanently_failed());
	CHECK(sim.res.stats().loading == 0);
	CHECK(sim.res.stats().resident == 0);
	CHECK(sim.drawn.empty());
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] coverage under streaming: no hole, no overlap and no partial cut while the budget binds") {
	// 6 levels: 31 interiors (1 page) + 32 leaves (2 pages) = 95 pages. Budget 60 percent.
	const uint32_t levels = 6;
	const uint32_t budget = 57;
	Sim sim;
	if (!sim.init(levels, budget, 2)) { FAIL("residency setup failed"); return; }
	sim.req.tau_pixels = 10;
	struct Pending { uint64_t due; Residency::Ticket ticket; };
	std::vector<Pending> in_flight;
	uint32_t loading_frames = 0, ready_frames = 0, loading_after_ready = 0, bad_frames = 0, over_budget = 0;
	uint32_t min_drawn = UINT32_MAX, max_drawn = 0;
	bool seen_ready = false;
	const uint32_t frames = 900;
	for (uint32_t f = 0; f < frames; ++f) {
		// Camera sweeps the line of leaves back and forth (x in [0, 320]).
		const float phase = float(f % 300) / 300.0f;
		sim.req.camera = Vector3((phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f) * 320.0f, 0, 0);
		// Payloads arrive two frames after they were requested. They land between frames, as a worker would.
		for (size_t i = 0; i < in_flight.size();) {
			if (in_flight[i].due <= sim.frame + 1) {
				sim.arrive(in_flight[i].ticket); // A ticket cancelled or evicted meanwhile is rejected by the model.
				in_flight.erase(in_flight.begin() + i);
			} else {
				++i;
			}
		}
		sim.step();
		for (uint32_t t = 0; t < sim.tickets.size(); ++t) { in_flight.push_back({ sim.frame + 2, sim.tickets[t] }); }
		if (sim.cut.status == CutHistory::COMPLETE) {
			seen_ready = true;
			++ready_frames;
			if (!every_drawn_resident(sim) || !exact_cover(sim, levels)) { ++bad_frames; }
			min_drawn = MIN(min_drawn, uint32_t(sim.drawn.size()));
			max_drawn = MAX(max_drawn, uint32_t(sim.drawn.size()));
		} else {
			++loading_frames;
			if (seen_ready) { ++loading_after_ready; }
			if (!sim.drawn.empty()) { ++bad_frames; } // A loading view never draws a partial cut.
		}
		const auto st = sim.res.stats();
		if (st.pages_resident + st.pages_in_flight + st.pages_retiring > budget) { ++over_budget; }
	}
	CHECK(bad_frames == 0);
	CHECK(over_budget == 0);
	CHECK(seen_ready);
	CHECK(loading_after_ready == 0); // The pinned root keeps a complete coarse cut available for ever.
	CHECK(loading_frames >= 1); // Cold start is reported as loading, separately from ready.
	CHECK(loading_frames + ready_frames == frames);
	// The sweep really refined and coarsened, and the budget really bound.
	CHECK(max_drawn > min_drawn);
	CHECK(max_drawn >= 8);
	CHECK(sim.res.stats().evicted_total > 0);
	CHECK(sim.res.stats().completed_total > 20);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a request that eviction cannot satisfy evicts nothing") {
	// 4 pages: root (1) + interior 1 (1) resident, 2 free. The leaf group {5, 6} needs 4 pages; evicting the
	// only unprotected node (interior 1) would make 3, so the group is skipped and interior 1 stays.
	Sim sim;
	if (!sim.init(3, 4, 0)) { FAIL("residency setup failed"); return; }
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 1, 2)) { FAIL("could not make nodes resident"); return; }
	sim.res.begin_frame(10);
	LocalVector<Residency::Ticket> tickets;
	const uint32_t group[2] = { 5, 6 };
	const auto result = sim.res.request(group, 2, tickets);
	CHECK(result.skipped_groups == 1);
	CHECK(result.evicted == 0);
	CHECK(tickets.size() == 0);
	CHECK(sim.res.state(1) == Residency::RESIDENT);
	CHECK(sim.res.stats().evicted_total == 0);
	CHECK(sim.res.stats().pages_in_flight == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a lower-priority group cannot evict the half-finished children of a higher-priority group") {
	// 4 pages: root and node 3 resident. Group 1 = {3, 4} (node 4 missing) outranks group 2 = {5, 6}. Node 3 is
	// not drawn (nothing is, the view is still loading), but it belongs to the first group.
	Sim sim;
	if (!one_page_tree(sim, 4)) { FAIL("residency setup failed"); return; }
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 3, 2)) { FAIL("could not make nodes resident"); return; }
	sim.res.begin_frame(10);
	LocalVector<Residency::Ticket> tickets;
	const uint32_t demand[4] = { 3, 4, 5, 6 };
	const auto result = sim.res.request(demand, 4, tickets);
	CHECK(result.requested == 1); // Only the missing child of group 1.
	CHECK(tickets.size() == 1);
	if (tickets.size() == 1) { CHECK(tickets[0].node == 4); }
	CHECK(result.skipped_groups == 1); // Group 2 needs 2 pages and only 1 is reclaimable without breaking group 1.
	CHECK(result.evicted == 0);
	CHECK(sim.res.state(3) == Residency::RESIDENT);
}

// #1483 B: no resident sibling is manually added to the selector's missing-only demand.
TEST_CASE("[GaussianSplatting][HLOD][Residency] missing-only selection demand protects a resident sibling through renewal and cut transfer (#1483 B)") {
	Sim sim;
	if (!one_page_tree(sim, 6)) { FAIL("residency setup failed"); return; }
	// Drawn parents 1/2 cover the whole tree. Child 3 is the oldest evictable resident and its sibling 4
	// is missing. There is room for 4, but not also for lower-priority children 5/6 without evicting 3.
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 3, 2) ||
			!make_resident_at(sim.res, 1, 3) || !make_resident_at(sim.res, 2, 4)) {
		FAIL("could not prepare the complete coarse cut and its resident child"); return;
	}
	sim.frame = 9;
	sim.step(false);
	CHECK(sim.cut.status == CutHistory::COMPLETE);
	CHECK(exact_cover(sim, 4));
	if (sim.demand != std::vector<uint32_t>({ 4, 5, 6 })) {
		FAIL("the real selector must emit only missing children, with parent 1 first"); return;
	}
	sim.request_result = sim.res.request(sim.demand.data(), uint32_t(sim.demand.size()), sim.tickets);
	CHECK(sim.request_result.requested == 1);
	CHECK(sim.request_result.evicted == 0);
	CHECK(sim.res.state(3) == Residency::RESIDENT);
	CHECK(sim.res.is_protected(3));
	CHECK(sim.res.is_protected(1));
	Residency::Ticket missing;
	for (const auto &ticket : sim.tickets) {
		CHECK(ticket.node == 4);
		if (ticket.node == 4) { missing = ticket; }
	}
	if (!missing.valid()) { FAIL("the admitted missing child did not receive a ticket"); return; }
	// Renew while 4 is still loading; neither the missing-only demand nor a new frame loses 3's lease.
	sim.step();
	CHECK(sim.res.state(3) == Residency::RESIDENT);
	CHECK(sim.res.is_protected(3));
	CHECK(sim.res.is_protected(1));
	CHECK(exact_cover(sim, 4));
	sim.arrive(missing);
	CHECK(sim.res.state(4) == Residency::RESIDENT);
	CHECK(sim.res.is_protected(4)); // Completion must retain protection until selection consumes it.
	CHECK_FALSE(sim.res.evict(4));
	sim.step();
	CHECK(sim.cut.status == CutHistory::COMPLETE);
	CHECK(sim.drawn == std::vector<uint32_t>({ 2, 3, 4 }));
	CHECK(every_drawn_resident(sim));
	CHECK(exact_cover(sim, 4));
	CHECK(sim.res.is_protected(3));
	CHECK(sim.res.is_protected(4));
	CHECK(sim.res.is_protected(1));
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a higher-priority arrival abandons lower tickets and protection without immediate readmission (#1483 B)") {
	Sim sim;
	if (!one_page_tree(sim, 3)) { FAIL("residency setup failed"); return; }
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 3, 2)) {
		FAIL("could not prepare a resident sibling"); return;
	}
	sim.res.begin_frame(10);
	const uint32_t lower[1] = { 4 };
	LocalVector<Residency::Ticket> initial;
	sim.res.request(lower, 1, initial);
	if (initial.size() != 1) { FAIL("the lower transition was not admitted"); return; }
	const Residency::Ticket abandoned = initial[0];
	bool uploading = false;
	SUBCASE("loading ticket") {}
	SUBCASE("partially uploaded ticket") {
		uploading = true;
		if (!sim.res.begin_upload(abandoned)) { FAIL("could not begin the lower upload"); return; }
	}
	sim.res.begin_frame(11);
	const uint32_t root[1] = { 0 };
	sim.res.protect_drawn(root, 1);
	// Emission order is the priority. The lower transition remains wanted by selection; the arbiter must
	// withdraw its executable demand itself. Duplicate emissions must not cancel or allocate twice.
	const uint32_t demand[6] = { 5, 6, 4, 5, 6, 4 };
	LocalVector<Residency::Ticket> head;
	sim.res.request(demand, 6, head);
	CHECK(sim.res.stats().cancelled_total == 1);
	CHECK(sim.res.stats().abandoned_transitions_total == 1);
	CHECK(sim.res.stats().abandoned_transitions_this_frame == 1);
	CHECK(sim.res.stats().transition_state == Residency::NORMAL);
	CHECK_FALSE(sim.res.is_pending(abandoned));
	CHECK(sim.res.state(4) == Residency::ABSENT);
	CHECK_FALSE(sim.res.is_protected(3));
	CHECK_FALSE(sim.res.is_protected(1));
	CHECK(sim.res.state(0) == Residency::RESIDENT);
	CHECK(sim.res.is_protected(0));
	CHECK(head.size() == 2);
	for (const auto &ticket : head) { CHECK((ticket.node == 5 || ticket.node == 6)); }
	// Exercise the actual generation gates, without calling cancel() on behalf of the implementation.
	if (uploading) { CHECK_FALSE(sim.res.finish_upload(abandoned, true)); }
	else { CHECK_FALSE(sim.res.begin_upload(abandoned)); }
	CHECK(sim.res.resident_mask()[4] == 0);
	CHECK(sim.res.stats().late_dropped_cancelled_total == 1);
	CHECK(sim.res.stats().late_dropped_total == 0);
	const uint64_t requested = sim.res.stats().requested_total;
	LocalVector<Residency::Ticket> repeated;
	sim.res.request(demand, 6, repeated);
	CHECK(repeated.is_empty());
	CHECK(sim.res.stats().requested_total == requested);
	CHECK(sim.res.stats().cancelled_total == 1);
	CHECK(sim.res.state(4) == Residency::ABSENT);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] full occupancy abandons lower work and admits the head after normal retirement (#1483 B)") {
	Sim sim;
	constexpr uint32_t retire_delay = 3;
	if (!one_page_tree(sim, 3, retire_delay)) { FAIL("residency setup failed"); return; }
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 3, 2)) {
		FAIL("could not prepare full-occupancy dependencies"); return;
	}
	sim.res.begin_frame(10);
	const uint32_t lower[1] = { 4 };
	LocalVector<Residency::Ticket> initial;
	sim.res.request(lower, 1, initial);
	if (initial.size() != 1) { FAIL("the lower transition was not admitted"); return; }
	CHECK(sim.res.stats().pages_free == 0);
	for (uint32_t node : { 0u, 3u, 4u }) { CHECK(sim.res.is_protected(node)); }
	const uint32_t demand[3] = { 5, 6, 4 };
	const uint32_t root[1] = { 0 };
	LocalVector<Residency::Ticket> head;
	// This is a synchronous model bound, not a poll of workers: allow cancellation retirement plus one
	// subsequent eviction retirement. The lower demand is deliberately offered on EVERY reconciliation.
	for (uint64_t frame = 11; frame <= 12 + 2 * retire_delay; ++frame) {
		sim.res.begin_frame(frame);
		sim.res.protect_drawn(root, 1);
		LocalVector<Residency::Ticket> tickets;
		sim.res.request(demand, 3, tickets);
		CHECK(sim.res.stats().cancelled_total == 1);
		CHECK_FALSE(sim.res.is_pending(initial[0]));
		for (const auto &ticket : tickets) {
			CHECK((ticket.node == 5 || ticket.node == 6));
			if (ticket.node == 5 || ticket.node == 6) { head.push_back(ticket); }
		}
		if (frame < 11 + retire_delay) {
			CHECK(head.is_empty());
			CHECK(sim.res.stats().transition_state == Residency::RETIREMENT_WAIT);
		}
		const auto stats = sim.res.stats();
		CHECK(stats.pages_resident + stats.pages_in_flight + stats.pages_retiring <= 3);
		CHECK(sim.res.state(0) == Residency::RESIDENT);
		CHECK(sim.res.state(4) == Residency::ABSENT);
	}
	if (head.size() != 2) { FAIL("the highest-priority whole transition did not progress after bounded retirement"); return; }
	for (const auto &ticket : head) { sim.arrive(ticket); }
	CHECK(sim.res.state(5) == Residency::RESIDENT);
	CHECK(sim.res.state(6) == Residency::RESIDENT);
	sim.frame = 12 + 2 * retire_delay;
	sim.step(false);
	CHECK(sim.cut.status == CutHistory::COMPLETE);
	CHECK(every_drawn_resident(sim));
	CHECK(exact_cover(sim, 4));
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] drawn fragmentation admits the whole transition and retains complete coverage (#1483 B, #1514)") {
	Sim sim;
	sim.data.instantiate();
	binary_tree(*sim.data.ptr(), 3, 1000, 1000);
	sim.data->nodes.write[3].topology.payload_count = 2500; // Three pages.
	sim.data->nodes.write[5].topology.payload_count = 1500;
	sim.data->nodes.write[6].topology.payload_count = 1500;
	Vector<Residency::NodeInfo> infos;
	if (!make_node_infos(*sim.data.ptr(), infos) || !sim.history.reserve(7) ||
			!sim.res.reset(infos, config(7, 0), 1, 1)) {
		FAIL("fragmented residency setup failed"); return;
	}
	// Pages: root@0, filler 5@1-2, drawn parent 1@3, filler 6@4-5, drawn parent 2@6.
	uint64_t frame = 1;
	for (uint32_t node : { 0u, 5u, 1u, 6u, 2u }) {
		if (!make_resident_at(sim.res, node, frame++)) { FAIL("could not arrange fragmented runs"); return; }
	}
	sim.res.begin_frame(20);
	LocalVector<Residency::Ticket> withdrawn;
	sim.res.request(nullptr, 0, withdrawn); // No transition from fixture construction still owns a lease.
	if (!sim.res.evict(5) || !sim.res.evict(6)) { FAIL("could not remove the two fillers"); return; }
	CHECK(sim.res.stats().pages_free == 4);
	CHECK(sim.res.stats().largest_free_run == 4);
	sim.frame = 20;
	sim.req.page_splats = 1024;
	sim.req.page_budget = 7; // The whole desired closure fits in the free pages.
	sim.step();
	if (sim.demand != std::vector<uint32_t>({ 3, 4 })) {
		FAIL("expected the highest-priority refinement demand"); return;
	}
	CHECK(sim.request_result.fragmented_groups == 0);
	CHECK(sim.tickets.size() == 2);
	CHECK(sim.request_result.evicted == 0);
	CHECK(sim.res.stats().allocation_refused_total == 0);
	CHECK(sim.res.stats().transition_state == Residency::NORMAL);
	sim.arrive_all();
	const auto placed = sim.res.stats();
	// Completed coverage stays stable without eviction or repeated requests.
	for (uint32_t retry = 0; retry < 12; ++retry) {
		sim.step();
		CHECK(sim.cut.status == CutHistory::COMPLETE);
		CHECK(every_drawn_resident(sim));
		CHECK(exact_cover(sim, 3));
		CHECK(sim.demand.empty()); // The desired cut was admitted and completed.
		CHECK(sim.tickets.is_empty());
		CHECK(sim.res.stats().pages_in_flight == 0);
		CHECK(sim.res.stats().evicted_total == placed.evicted_total);
		CHECK(sim.res.stats().requested_total == placed.requested_total);
		CHECK(sim.res.stats().transition_state == Residency::NORMAL);
	}
	CHECK(sim.res.stats().allocation_refused_total == placed.allocation_refused_total);
}


TEST_CASE("[GaussianSplatting][HLOD][Residency] a permanently failed arbitration head releases soft leases and lower suppression (#1483 B)") {
	GaussianSplatHlodRuntimeData data;
	binary_tree(data, 4, 1000, 1000);
	Vector<Residency::NodeInfo> infos;
	Residency res;
	auto cfg = config(4, 0);
	cfg.max_failures = 1;
	if (!make_node_infos(data, infos) || !res.reset(infos, cfg, 1, 1) ||
			!make_resident_at(res, 0, 1) || !make_resident_at(res, 3, 2)) {
		FAIL("failed-head residency setup failed"); return;
	}
	res.begin_frame(10);
	const uint32_t lower[1] = { 4 };
	LocalVector<Residency::Ticket> initial;
	res.request(lower, 1, initial);
	if (initial.size() != 1) { FAIL("lower transition was not admitted"); return; }
	// Two new children cannot fit until the lower transition is explicitly abandoned. This creates
	// an actual arbitration head and suppressed lower work, rather than only an ordinary failed group.
	res.begin_frame(11);
	const uint32_t demand[3] = { 5, 6, 4 };
	LocalVector<Residency::Ticket> head;
	res.request(demand, 3, head);
	if (head.size() != 2) { FAIL("arbitration head was not admitted"); return; }
	CHECK(res.stats().abandoned_transitions_total == 1);
	Residency::Ticket failed, sibling;
	for (const auto &ticket : head) {
		if (ticket.node == 5) { failed = ticket; }
		if (ticket.node == 6) { sibling = ticket; }
	}
	if (!failed.valid() || !sibling.valid()) { FAIL("head did not contain the intended sibling pair"); return; }
	if (!res.begin_upload(sibling) || !res.finish_upload(sibling, true) || !res.fail_load(failed)) {
		FAIL("could not complete one sibling and permanently fail the other"); return;
	}
	CHECK(res.stats().permanently_failed == 1);
	bool drawn_sibling = false;
	SUBCASE("resident sibling had only a revocable lease") {}
	SUBCASE("resident sibling is also drawn") { drawn_sibling = true; }
	res.begin_frame(12);
	if (drawn_sibling) {
		const uint32_t drawn[1] = { 6 };
		res.protect_drawn(drawn, 1);
	}
	LocalVector<Residency::Ticket> eligible_lower;
	const auto result = res.request(demand, 3, eligible_lower);
	CHECK(result.blocked_groups == 1);
	CHECK(result.skipped_groups == 0);
	CHECK(result.fragmented_groups == 0);
	CHECK(result.pending_retire_groups == 0);
	CHECK(result.requested == 1);
	if (eligible_lower.size() != 1) { FAIL("failed head left eligible lower work suppressed"); return; }
	CHECK(eligible_lower[0].node == 4);
	CHECK(res.state(5) == Residency::FAILED);
	CHECK(res.state(6) == Residency::RESIDENT);
	CHECK(res.is_protected(6) == drawn_sibling);
	CHECK(res.is_protected(2) == drawn_sibling);
	CHECK(res.stats().protected_sibling_nodes == 1); // Only resident child 3 of the renewed lower group.
	CHECK(res.stats().transition_state == Residency::NORMAL);
	CHECK(res.stats().abandoned_transitions_total == 1); // Failure withdrawal is not priority abandonment.
	CHECK(res.stats().abandoned_transitions_this_frame == 0);
	CHECK(res.evict(6) == !drawn_sibling);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] native transition telemetry distinguishes absence measured idle values and reset (#1483 B)") {
	CHECK(int(Residency::TRANSITIONS_ABSENT) == -1);
	CHECK(int(Residency::NORMAL) == 0);
	CHECK(int(Residency::RETIREMENT_WAIT) == 1);
	CHECK(int(Residency::COARSE_PLACEMENT_BLOCKED) == 2);
	CHECK(int(Residency::UNSUPPORTED_OWNERS) == 3);
	Residency absent;
	const auto unavailable = absent.stats();
	CHECK_FALSE(unavailable.transitions_measured);
	CHECK(unavailable.transition_state == Residency::TRANSITIONS_ABSENT);
	Sim sim;
	if (!one_page_tree(sim, 3)) { FAIL("residency setup failed"); return; }
	const auto idle = sim.res.stats();
	CHECK(idle.transitions_measured);
	CHECK(idle.transition_state == Residency::NORMAL);
	CHECK(idle.protected_sibling_nodes == 0);
	CHECK(idle.abandoned_transitions_total == 0);
	CHECK(idle.abandoned_transitions_this_frame == 0);
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 3, 2)) {
		FAIL("could not prepare a measured sibling lease"); return;
	}
	sim.res.begin_frame(10);
	const uint32_t lower[1] = { 4 };
	LocalVector<Residency::Ticket> initial;
	sim.res.request(lower, 1, initial);
	if (initial.size() != 1) { FAIL("lower transition was not admitted"); return; }
	CHECK(sim.res.stats().protected_sibling_nodes == 1);
	sim.res.begin_frame(11);
	const uint32_t demand[3] = { 5, 6, 4 };
	LocalVector<Residency::Ticket> head;
	sim.res.request(demand, 3, head);
	const auto measured = sim.res.stats();
	CHECK(measured.transitions_measured);
	CHECK(measured.protected_sibling_nodes == 0);
	CHECK(measured.abandoned_transitions_total == 1);
	CHECK(measured.abandoned_transitions_this_frame == 1);
	CHECK(measured.transition_state == Residency::NORMAL);
	sim.res.begin_frame(12);
	CHECK(sim.res.stats().abandoned_transitions_total == 1);
	CHECK(sim.res.stats().abandoned_transitions_this_frame == 0);
	CHECK(measured.abandoned_transitions_this_frame == 1); // A retained native snapshot is immutable.
	Vector<Residency::NodeInfo> infos;
	if (!make_node_infos(*sim.data.ptr(), infos) || !sim.res.reset(infos, config(3, 0), 2, 2)) {
		FAIL("residency reset failed"); return;
	}
	const auto reset = sim.res.stats();
	CHECK(reset.transitions_measured);
	CHECK(reset.transition_state == Residency::NORMAL);
	CHECK(reset.protected_sibling_nodes == 0);
	CHECK(reset.abandoned_transitions_total == 0);
	CHECK(reset.abandoned_transitions_this_frame == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] explicit transition visibility leases resident visible siblings only (#1483 B)") {
	Vector<Residency::NodeInfo> infos;
	infos.resize(4); // Root with three children; child 3 is outside this view's frustum.
	for (uint32_t i = 0; i < 4; ++i) {
		infos.write[i].splat_count = 1000;
		infos.write[i].parent = i == 0 ? UINT32_MAX : 0;
		infos.write[i].height = i == 0 ? 1 : 0;
	}
	Residency res;
	auto cfg = config(4, 0);
	cfg.pin_root = false; // The transition itself must protect its parent in both subcases.
	if (!res.reset(infos, cfg, 1, 1)) { FAIL("residency setup failed"); return; }
	for (uint32_t node : { 0u, 1u, 3u }) {
		if (!make_resident_at(res, node, node + 1)) { FAIL("could not prepare resident siblings"); return; }
	}
	res.begin_frame(10);
	const uint32_t missing[1] = { 2 };
	TransitionGroup transition[1] = { { 0, 1, 0, 3, TRANSITION_REFINEMENT } };
	SUBCASE("ordinary refinement") {}
	SUBCASE("bootstrap also preserves its visible resident child") { transition[0].kind = TRANSITION_BOOTSTRAP; }
	LocalVector<Residency::Ticket> tickets;
	const auto result = res.request(missing, 1, tickets, transition);
	CHECK(result.requested == 1);
	CHECK(res.is_protected(0));
	CHECK(res.is_protected(1));
	CHECK_FALSE(res.is_protected(3));
	CHECK(res.stats().protected_sibling_nodes == 1);
	CHECK_FALSE(res.evict(0));
	CHECK_FALSE(res.evict(1));
	CHECK(res.evict(3)); // The invisible resident remains reclaimable under the explicit visible mask.
	res.withdraw_transitions();
	CHECK_FALSE(res.is_protected(0));
	CHECK(res.evict(0));
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] drawn protection survives revocation of an overlapping sibling lease (#1483 B)") {
	Sim sim;
	if (!one_page_tree(sim, 4)) { FAIL("residency setup failed"); return; }
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 3, 2)) {
		FAIL("could not prepare drawn sibling"); return;
	}
	sim.res.begin_frame(10);
	const uint32_t lower[1] = { 4 };
	LocalVector<Residency::Ticket> initial;
	sim.res.request(lower, 1, initial);
	if (initial.size() != 1) { FAIL("lower transition was not admitted"); return; }
	sim.res.begin_frame(11);
	const uint32_t drawn[1] = { 3 };
	sim.res.protect_drawn(drawn, 1);
	const uint32_t demand[3] = { 5, 6, 4 };
	LocalVector<Residency::Ticket> head;
	sim.res.request(demand, 3, head);
	CHECK(head.size() == 2);
	CHECK(sim.res.stats().abandoned_transitions_total == 1);
	CHECK(sim.res.stats().protected_sibling_nodes == 0);
	CHECK_FALSE(sim.res.is_pending(initial[0]));
	CHECK(sim.res.is_protected(3));
	CHECK(sim.res.is_protected(1)); // Ancestor protection belongs to the actual drawn node.
	CHECK_FALSE(sim.res.evict(3));
	// No drawn renewal next frame: the old soft lease must not keep the node protected either.
	sim.res.begin_frame(12);
	const uint32_t higher[2] = { 5, 6 };
	LocalVector<Residency::Ticket> repeated;
	sim.res.request(higher, 2, repeated);
	CHECK_FALSE(sim.res.is_protected(3));
	CHECK_FALSE(sim.res.is_protected(1));
	CHECK(sim.res.evict(3));
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] bootstrap keeps are revocable bundle ownership and a stale root never gains a keep lease (#1483 B)") {
	Sim sim;
	if (!one_page_tree(sim, 4)) { FAIL("residency setup failed"); return; }
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 3, 2)) {
		FAIL("could not prepare bootstrap keeps"); return;
	}
	sim.res.begin_frame(10);
	const uint32_t demand[1] = { 4 }, kept[1] = { 3 };
	const TransitionGroup transition[1] = { { 1, 3, 0, 3, TRANSITION_BOOTSTRAP } };
	LocalVector<Residency::Ticket> tickets;
	sim.res.request(demand, 1, tickets, transition, kept, 1);
	if (tickets.size() != 1) { FAIL("bootstrap missing child was not admitted"); return; }
	CHECK(sim.res.is_protected(3));
	CHECK_FALSE(sim.res.evict(3));
	CHECK(sim.res.stats().protected_sibling_nodes == 1);
	sim.res.withdraw_transitions();
	CHECK_FALSE(sim.res.is_protected(3));
	CHECK(sim.res.stats().protected_sibling_nodes == 0);
	CHECK_FALSE(sim.res.is_pending(tickets[0]));
	CHECK(sim.res.evict(3)); // A keep did not acquire a non-revocable drawn stamp.
	Vector<Residency::NodeInfo> infos;
	if (!make_node_infos(*sim.data.ptr(), infos)) { FAIL("could not make stale residency table"); return; }
	infos.write[0].stale = true;
	if (!sim.res.reset(infos, config(4, 0), 2, 2)) { FAIL("stale residency reset failed"); return; }
	sim.res.begin_frame(20);
	const uint32_t stale_keep[1] = { 0 };
	LocalVector<Residency::Ticket> refused;
	const auto result = sim.res.request(nullptr, 0, refused, nullptr, stale_keep, 1);
	CHECK(result.blocked_groups == 1);
	CHECK(refused.is_empty());
	CHECK_FALSE(sim.res.is_protected(0));
	CHECK(sim.res.stats().protected_sibling_nodes == 0);
}

static bool bootstrap_cap_fixture(Residency &r_res, LocalVector<uint32_t> &r_frontier, uint32_t p_count) {
	GaussianSplatHlodRuntimeData data;
	binary_tree(data, 8, 1000, 1000); // Valid binary topology, one page per node.
	Vector<Residency::NodeInfo> infos;
	if (!make_node_infos(data, infos) || !r_res.reset(infos, config(p_count + 1, 0), 1, 1) ||
			!make_resident_at(r_res, 0, 1)) {
		return false;
	}
	// Start with a complete 32/64-node frontier. Refining eight of the 64 nodes
	// replaces eight parents with sixteen children: a complete 72-node plan.
	const uint32_t base_count = p_count > 64 ? 64 : p_count;
	const uint32_t first = base_count - 1;
	for (uint32_t node = first; node < first + base_count; ++node) {
		if (node - first < p_count - base_count) {
			r_frontier.push_back(2 * node + 1);
			r_frontier.push_back(2 * node + 2);
		} else {
			r_frontier.push_back(node);
		}
	}
	return r_frontier.size() == p_count;
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a 72-request bootstrap reserves the whole plan and progresses across the unchanged frame cap (#1483 B)") {
	constexpr uint32_t count = 72, cap = 64;
	Residency res;
	LocalVector<uint32_t> frontier, kept;
	if (!bootstrap_cap_fixture(res, frontier, count)) { FAIL("could not prepare the 72-node bootstrap frontier"); return; }
	CHECK(res.config().max_requests_per_frame == cap);
	LocalVector<TransitionGroup> transitions;
	transitions.resize(count);
	for (auto &transition : transitions) { transition.kind = TRANSITION_BOOTSTRAP; }
	kept.push_back(0); // Complete coarse coverage stays resident while the bootstrap fills.
	res.begin_frame(10);
	LocalVector<Residency::Ticket> first;
	const auto started = res.request(frontier.ptr(), count, first, transitions.ptr(), kept.ptr(), kept.size());
	CHECK(started.requested == cap);
	CHECK(started.capped);
	if (first.size() != cap) { FAIL("bootstrap must use the available 64-request frame budget, got ", first.size()); return; }
	std::vector<Vector<uint32_t>> reserved_pages(count);
	std::vector<uint32_t> reserved_bases(count);
	for (uint32_t i = 0; i < count; ++i) {
		if (!res.page_allocation(frontier[i], reserved_pages[i], reserved_bases[i])) {
			FAIL("bootstrap did not reserve every run before starting its first request batch"); return;
		}
		CHECK(reserved_pages[i].size() == 1);
		CHECK(res.is_protected(frontier[i]));
	}
	CHECK(res.stats().pages_free == 0); // The eight deferred requests already own reserved runs.
	for (uint32_t i = 0; i < cap; ++i) {
		CHECK(first[i].node == frontier[i]);
		CHECK(first[i].pages == reserved_pages[i]);
		CHECK(first[i].pt_base == reserved_bases[i]);
		if (!res.begin_upload(first[i]) || !res.finish_upload(first[i], true)) {
			FAIL("first bootstrap batch could not become resident"); return;
		}
		kept.push_back(frontier[i]);
	}
	// Real selection now emits only the eight missing nodes and keeps the completed prefix.
	// Reconciliation in the SAME logical frame must preserve reservations without exceeding 64.
	LocalVector<Residency::Ticket> repeated;
	const auto same_frame = res.request(frontier.ptr() + cap, count - cap, repeated, transitions.ptr() + cap, kept.ptr(), kept.size());
	CHECK(same_frame.requested == 0);
	CHECK(same_frame.capped);
	CHECK(repeated.is_empty());
	CHECK(res.stats().requested_total == 1 + cap); // Includes the fixture's root request.
	CHECK(res.stats().resident == 1 + cap);
	CHECK(res.stats().pages_free == 0);
	for (uint32_t i = 0; i < cap; ++i) {
		CHECK(res.is_protected(frontier[i]));
		CHECK_FALSE(res.evict(frontier[i])); // Completed prefix remains leased to the whole plan.
	}
	// These are synchronous model frames, not a fixed-frame wait for worker progress.
	res.begin_frame(11);
	LocalVector<Residency::Ticket> last;
	const auto resumed = res.request(frontier.ptr() + cap, count - cap, last, transitions.ptr() + cap, kept.ptr(), kept.size());
	CHECK(resumed.requested == count - cap);
	CHECK_FALSE(resumed.capped);
	if (last.size() != count - cap) { FAIL("remaining eight bootstrap requests did not start in the next frame"); return; }
	for (uint32_t i = 0; i < last.size(); ++i) {
		CHECK(last[i].node == frontier[cap + i]);
		CHECK(last[i].pages == reserved_pages[cap + i]); // The reservation does not move.
		CHECK(last[i].pt_base == reserved_bases[cap + i]);
		CHECK(last[i].pages.size() == 1);
		if (!res.begin_upload(last[i]) || !res.finish_upload(last[i], true)) {
			FAIL("final bootstrap batch could not become resident"); return;
		}
	}
	LocalVector<Residency::Ticket> complete;
	const uint32_t root = 0;
	const auto settled = res.request(frontier.ptr(), count, complete, transitions.ptr(), &root, 1);
	CHECK(settled.requested == 0);
	CHECK(settled.satisfied == count + 1);
	CHECK(complete.is_empty());
	CHECK(res.stats().requested_total == count + 1);
	CHECK(res.stats().completed_total == count + 1);
	CHECK(res.stats().resident == count + 1);
	CHECK(res.stats().loading == 0);
	CHECK(res.stats().uploading == 0);
	CHECK(res.stats().pages_free == 0);
	CHECK(res.stats().cancelled_total == 0);
	CHECK(res.stats().abandoned_transitions_total == 0);
	for (uint32_t node : frontier) {
		CHECK(res.state(node) == Residency::RESIDENT);
		CHECK(res.resident_mask()[node] == 1);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] bootstrap plans within 64 requests keep their tickets in the same frame (#1483 B)") {
	for (uint32_t count : { 32u, 64u }) {
		CAPTURE(count);
		Residency res;
		LocalVector<uint32_t> frontier;
		if (!bootstrap_cap_fixture(res, frontier, count)) { FAIL("could not prepare the within-cap bootstrap frontier"); return; }
		CHECK(res.config().max_requests_per_frame == 64);
		LocalVector<TransitionGroup> transitions;
		transitions.resize(count);
		for (auto &transition : transitions) { transition.kind = TRANSITION_BOOTSTRAP; }
		const uint32_t root = 0;
		res.begin_frame(10);
		LocalVector<Residency::Ticket> tickets;
		const auto started = res.request(frontier.ptr(), count, tickets, transitions.ptr(), &root, 1);
		CHECK(started.requested == count);
		CHECK_FALSE(started.capped);
		if (tickets.size() != count) { FAIL("within-cap bootstrap did not emit all its tickets in frame 10"); return; }
		for (uint32_t i = 0; i < count; ++i) {
			const auto &ticket = tickets[i];
			CHECK(ticket.node == frontier[i]);
			CHECK(res.allocation_matches(ticket));
			CHECK(ticket.pages.size() == 1);
			CHECK(ticket.request_generation == i + 2); // Root was the sole earlier request.
			CHECK(ticket.world_generation == 1);
			CHECK(ticket.device_generation == 1);
			CHECK_FALSE(ticket.shadow);
			CHECK(res.is_pending(ticket));
			if (!res.begin_upload(ticket) || !res.finish_upload(ticket, true)) {
				FAIL("within-cap bootstrap ticket did not finish in its original frame"); return;
			}
		}
		CHECK(res.frame() == 10);
		CHECK(res.stats().resident == count + 1);
		CHECK(res.stats().completed_total == count + 1);
		CHECK(res.stats().pages_free == 0);
		LocalVector<Residency::Ticket> repeated;
		const auto settled = res.request(frontier.ptr(), count, repeated, transitions.ptr(), &root, 1);
		CHECK(settled.requested == 0);
		CHECK_FALSE(settled.capped);
		CHECK(repeated.is_empty());
		CHECK(res.stats().requested_total == count + 1);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] mass transition abandonment classifies every distinct late ticket beyond the legacy cancel window (#1483 B)") {
	GaussianSplatHlodRuntimeData data;
	binary_tree(data, 10, 1000, 1000);
	Vector<Residency::NodeInfo> infos;
	Residency res;
	if (!make_node_infos(data, infos) || !res.reset(infos, config(513, 0), 1, 1) || !make_resident_at(res, 0, 1)) {
		FAIL("mass abandonment setup failed"); return;
	}
	std::vector<uint32_t> lower(512);
	for (uint32_t i = 0; i < 512; ++i) { lower[i] = 511 + i; }
	LocalVector<Residency::Ticket> pending;
	// Synchronous request-cap accounting: 64 requests per frame, eight reconciliations, no workers.
	for (uint32_t pass = 0; pass < 8; ++pass) {
		res.begin_frame(10 + pass);
		LocalVector<Residency::Ticket> tickets;
		const auto result = res.request(lower.data(), uint32_t(lower.size()), tickets);
		CHECK(result.requested == 64);
		for (const auto &ticket : tickets) { pending.push_back(ticket); }
		CHECK(res.stats().cancelled_total == 0);
		CHECK(res.stats().pages_in_flight == (pass + 1) * 64);
	}
	if (pending.size() != 512) { FAIL("expected 512 distinct pending leaf tickets, got ", pending.size()); return; }
	for (uint32_t i = 0; i < pending.size(); ++i) { CHECK(pending[i].node == 511 + i); }
	CHECK(res.stats().pages_free == 0);
	std::vector<uint32_t> demand = { 1, 2 };
	demand.insert(demand.end(), lower.begin(), lower.end());
	res.begin_frame(20);
	const uint32_t drawn[1] = { 0 };
	res.protect_drawn(drawn, 1);
	LocalVector<Residency::Ticket> head;
	const auto result = res.request(demand.data(), uint32_t(demand.size()), head);
	CHECK(result.requested == 2);
	if (head.size() != 2) { FAIL("highest-priority whole transition did not progress"); return; }
	CHECK(head[0].node == 1);
	CHECK(head[1].node == 2);
	CHECK(res.stats().cancelled_total == 512);
	CHECK(res.stats().abandoned_transitions_total == 256);
	CHECK(res.stats().abandoned_transitions_this_frame == 256);
	CHECK(res.stats().transition_state == Residency::NORMAL);
	for (const auto &ticket : pending) {
		if (ticket.node < 511 || ticket.node >= 1023) { FAIL("invalid pending leaf ticket ", ticket.node); return; }
		CHECK_FALSE(res.is_pending(ticket));
		CHECK_FALSE(res.begin_upload(ticket));
		CHECK(res.resident_mask()[ticket.node] == 0);
	}
	CHECK(res.stats().late_dropped_cancelled_total == 512);
	CHECK(res.stats().late_dropped_total == 0);
	CHECK_FALSE(res.begin_upload(pending[0])); // The second delivery is real duplicate loss.
	CHECK(res.stats().late_dropped_cancelled_total == 512);
	CHECK(res.stats().late_dropped_total == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] duplicate demand is one demand and never leaks a run") {
	Sim sim;
	if (!sim.init(3, 16)) { FAIL("residency setup failed"); return; }
	if (!make_resident_at(sim.res, 0, 1)) { FAIL("could not make root resident"); return; }
	const uint32_t free_before = sim.res.stats().pages_free;
	sim.res.begin_frame(2);
	LocalVector<Residency::Ticket> tickets;
	// The bootstrap demanded the root's children and the refinement walk demanded them again.
	const uint32_t demand[4] = { 1, 2, 1, 2 };
	const auto result = sim.res.request(demand, 4, tickets);
	CHECK(result.requested == 2);
	CHECK(tickets.size() == 2);
	const auto stats = sim.res.stats();
	CHECK(stats.pages_in_flight == sim.res.pages_for(1) + sim.res.pages_for(2));
	CHECK(stats.pages_free == free_before - stats.pages_in_flight); // Nothing allocated twice.
	// Every page comes back: cancel both, let the runs retire, and the atlas is as before.
	for (uint32_t i = 0; i < tickets.size(); ++i) { CHECK(sim.res.cancel(tickets[i].node)); }
	sim.res.begin_frame(20);
	CHECK(sim.res.stats().pages_free == free_before);
	// A second request in the same state does not re-allocate a node that already has a run.
	sim.res.begin_frame(21);
	tickets.clear();
	sim.res.request(demand, 4, tickets);
	LocalVector<Residency::Ticket> again;
	sim.res.request(demand, 4, again);
	CHECK(again.size() == 0);
	CHECK(sim.res.stats().pages_free == free_before - sim.res.stats().pages_in_flight);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a node evicted and requested again in one pass is not reported as evicted") {
	Sim sim;
	if (!one_page_tree(sim, 6)) { FAIL("residency setup failed"); return; }
	for (uint32_t n : { 0u, 7u, 8u, 9u, 10u }) {
		if (!make_resident_at(sim.res, n, 5)) { FAIL("could not make node resident"); return; }
	}
	sim.res.begin_frame(20);
	LocalVector<Residency::Ticket> tickets;
	// Group 1 {13, 14} evicts leaf 7 for its second page; group 2 {7, 8} (children of 3) wants 7 back, which
	// in turn evicts leaf 9.
	const uint32_t demand[4] = { 13, 14, 7, 8 };
	const auto result = sim.res.request(demand, 4, tickets);
	CHECK(result.evicted >= 2);
	bool has_seven = false;
	for (uint32_t i = 0; i < tickets.size(); ++i) { has_seven = has_seven || tickets[i].node == 7; }
	CHECK(has_seven);
	LocalVector<uint32_t> evicted;
	sim.res.take_evicted(evicted);
	// The glue zeroes the GPU metadata of exactly these nodes: 7 is being loaded again and must not be among them.
	for (uint32_t i = 0; i < evicted.size(); ++i) { CHECK(evicted[i] != 7u); }
	CHECK(evicted.size() >= 1);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a group skipped only because its pages are retiring still outranks lower-priority groups") {
	Sim sim;
	if (!one_page_tree(sim, 4, 3)) { FAIL("residency setup failed"); return; }
	if (!make_resident_at(sim.res, 0, 1) || !make_resident_at(sim.res, 3, 2) || !make_resident_at(sim.res, 9, 5) || !make_resident_at(sim.res, 10, 5)) {
		FAIL("could not make nodes resident");
		return;
	}
	sim.res.begin_frame(10);
	sim.res.withdraw_transitions(); // Completed fixture loads must not own the bystander's transition.
	CHECK(sim.res.evict(9)); // One page is now retiring (3 frames), none free.
	CHECK(sim.res.stats().pages_free == 0);
	CHECK(sim.res.stats().pages_retiring == 1);
	LocalVector<Residency::Ticket> tickets;
	// Group 1 = {3, 4}: node 3 is half of it, node 4 needs the retiring page. Group 2 = {5, 6} is lower priority.
	const uint32_t demand[4] = { 3, 4, 5, 6 };
	// Group 1 alone: its page is still retiring, so it waits. Evicting a bystander elsewhere (node 10) would
	// not bring the page back any sooner.
	LocalVector<Residency::Ticket> first_only;
	const auto first = sim.res.request(demand, 2, first_only);
	CHECK(first.requested == 0);
	CHECK(first.evicted == 0);
	CHECK(sim.res.state(10) == Residency::RESIDENT);
	const auto result = sim.res.request(demand, 4, tickets);
	CHECK(result.requested == 0); // Nothing can be placed until the retiring page is released.
	CHECK(result.pending_retire_groups >= 1);
	CHECK(sim.res.state(3) == Residency::RESIDENT); // Group 2 must not take group 1's half-finished child.
	// Once the page retires, group 1 completes first.
	sim.res.begin_frame(13);
	tickets.clear();
	sim.res.request(demand, 4, tickets);
	bool group_one = false;
	for (uint32_t i = 0; i < tickets.size(); ++i) { group_one = group_one || tickets[i].node == 4; }
	CHECK(group_one);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] the resident frontier kept by an incomplete bootstrap is protected, only the rest is demand") {
	using gs_hlod_runtime::DesiredRecord;
	using gs_hlod_runtime::DrawRecord;
	// Splitting: drawn and kept nodes are protected, the others are demand, order preserved.
	DesiredRecord want[3];
	want[0].node = 13; want[0].keep = false;
	want[1].node = 3; want[1].keep = true;
	want[2].node = 14; want[2].keep = false;
	DrawRecord drawn[1];
	drawn[0].node = 9;
	LocalVector<uint32_t> prot, dem;
	HlodNodeStreamer::split_protection_and_demand(want, 3, drawn, 1, prot, dem);
	if (prot.size() != 2 || dem.size() != 2) { FAIL("unexpected split sizes ", prot.size(), " ", dem.size()); return; }
	CHECK(prot[0] == 9);
	CHECK(prot[1] == 3);
	CHECK(dem[0] == 13);
	CHECK(dem[1] == 14);
	// Through the model: 3 pages, root + kept node 3 + unprotected leaf 8 resident. The 2-page group {13, 14}
	// cannot be paid for by evicting leaf 8 alone, and the kept node must not be the second victim.
	Sim sim;
	if (!one_page_tree(sim, 3)) { FAIL("residency setup failed"); return; }
	for (uint32_t n : { 0u, 3u, 8u }) {
		if (!make_resident_at(sim.res, n, 5)) { FAIL("could not make node resident"); return; }
	}
	sim.res.begin_frame(20);
	sim.res.protect_drawn(prot.ptr(), prot.size());
	LocalVector<Residency::Ticket> tickets;
	sim.res.request(dem.ptr(), dem.size(), tickets);
	CHECK(sim.res.state(3) == Residency::RESIDENT);
	CHECK(sim.res.state(8) == Residency::RESIDENT); // Futile: nothing is evicted for a group that cannot fit.
	CHECK(tickets.size() == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a failed child remains blocked while fragmented free pages admit without neighbour eviction") {
	// Blocked: a child failed permanently, so the transition can never complete.
	{
		Sim sim;
		if (!sim.init(3, 32)) { FAIL("residency setup failed"); return; }
		if (!make_resident_at(sim.res, 0, 1)) { FAIL("could not make root resident"); return; }
		LocalVector<Residency::Ticket> tickets;
		uint64_t frame = 2;
		for (int attempt = 0; attempt < 3; ++attempt) {
			tickets.clear();
			sim.res.begin_frame(frame += 100);
			const uint32_t group[2] = { 1, 2 };
			sim.res.request(group, 2, tickets);
			for (uint32_t i = 0; i < tickets.size(); ++i) {
				if (tickets[i].node == 1) { sim.res.fail_load(tickets[i]); } else { sim.res.cancel(tickets[i].node); }
			}
		}
		CHECK(sim.res.stats().permanently_failed == 1);
		sim.res.begin_frame(frame + 100);
		tickets.clear();
		const uint32_t group[2] = { 1, 2 };
		const auto result = sim.res.request(group, 2, tickets);
		CHECK(result.blocked_groups == 1);
		CHECK(result.fragmented_groups == 0);
		CHECK(tickets.size() == 0);
	}
	// Fragmented: pages 1-2 and 4 are free (3 in total, largest run 2); node 3 needs 3 pages.
	// Node 4 sits at page 3 between the holes. (Node 3's parent is 1, so node 4 is not its sibling.)
	auto fragmented_atlas = [](Residency &r_res) {
		Vector<Residency::NodeInfo> infos;
		infos.resize(5);
		const uint32_t splats[5] = { 1000, 1000, 1500, 2500, 1000 }; // 1, 1, 2, 3, 1 pages
		const uint32_t parents[5] = { UINT32_MAX, 0, 0, 1, 0 };
		for (int i = 0; i < 5; ++i) {
			infos.write[i].splat_count = splats[i];
			infos.write[i].parent = parents[i];
			infos.write[i].height = i == 0 ? 1 : 0;
		}
		if (!r_res.reset(infos, config(5, 0), 1, 1)) { return false; }
		if (!make_resident_at(r_res, 0, 1) || !make_resident_at(r_res, 2, 2) || !make_resident_at(r_res, 4, 3)) { return false; }
		r_res.begin_frame(10);
		r_res.withdraw_transitions(); // Arrange free runs after ending the fixture's admitted transition.
		return r_res.evict(2); // Holes: pages 1-2 and page 4.
	};
	{
		Residency res;
		if (!fragmented_atlas(res)) { FAIL("fragmented atlas setup failed"); return; }
		CHECK(res.stats().pages_free == 3);
		CHECK(res.stats().largest_free_run == 3);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t want[1] = { 3 };
		const auto result = res.request(want, 1, tickets);
		// Scattered pages satisfy demand without evicting the neighbour.
		CHECK(result.evicted == 0);
		CHECK(result.requested == 1);
		CHECK(res.state(4) == Residency::RESIDENT);
		CHECK(res.state(3) == Residency::LOADING);
		CHECK(res.state(0) == Residency::RESIDENT);
	}
	// The same admission succeeds while the neighbour is drawn and protected.
	{
		Residency res;
		if (!fragmented_atlas(res)) { FAIL("fragmented atlas setup failed"); return; }
		const uint32_t drawn[1] = { 4 };
		res.protect_drawn(drawn, 1);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t want[1] = { 3 };
		const auto result = res.request(want, 1, tickets);
		CHECK(result.skipped_groups == 0);
		CHECK(result.fragmented_groups == 0);
		CHECK(result.blocked_groups == 0);
		CHECK(result.evicted == 0);
		CHECK(tickets.size() == 1);
		CHECK(res.state(4) == Residency::RESIDENT);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] at the production retire delay scattered pages admit together after retirement without bystander eviction") {
	// Cap 8, retire delay 3. Pages: p0 root, then filler/bystander pairs, so after the fillers retire the free
	// pages are single holes (p1, p3, p5, p7) with a bystander between each: 4 pages free, no run of 2.
	Vector<Residency::NodeInfo> infos;
	infos.resize(11);
	const uint32_t parents[11] = { UINT32_MAX, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
	const uint32_t splats[11] = { 1000, 1000, 1500, 1500, 1000, 1000, 1000, 1000, 1000, 1000, 1000 }; // 2, 3: two pages
	for (int i = 0; i < 11; ++i) {
		infos.write[i].splat_count = splats[i];
		infos.write[i].parent = parents[i];
		infos.write[i].height = i == 0 ? 1 : 0;
	}
	Residency res;
	if (!res.reset(infos, config(8, 3), 1, 1)) { FAIL("residency setup failed"); return; }
	uint64_t frame = 1;
	for (uint32_t n : { 0u, 7u, 4u, 8u, 5u, 9u, 6u, 10u }) {
		if (!make_resident_at(res, n, frame++)) { FAIL("could not make node resident"); return; }
	}
	res.begin_frame(20);
	res.withdraw_transitions(); // Fillers are completed setup data, not an admitted sibling group under test.
	for (uint32_t n : { 7u, 8u, 9u, 10u }) { CHECK(res.evict(n)); }
	LocalVector<Residency::Ticket> tickets;
	const uint32_t group[2] = { 2, 3 };
	// Retirement still owns all four pages; no extra eviction is needed while they retire.
	auto result = res.request(group, 2, tickets);
	CHECK(result.requested == 0);
	CHECK(result.evicted == 0);
	CHECK(result.pending_retire_groups == 1);
	for (uint64_t frame : { 21u, 22u }) {
		res.begin_frame(frame);
		result = res.request(group, 2, tickets);
		CHECK(result.requested == 0);
		CHECK(result.evicted == 0);
		CHECK(tickets.is_empty());
	}
	res.begin_frame(23); // All four isolated pages are now free.
	CHECK(res.stats().pages_free == 4);
	CHECK(res.stats().largest_free_run == 4);
	result = res.request(group, 2, tickets);
	CHECK(result.requested == 2);
	CHECK(result.evicted == 0);
	CHECK(result.fragmented_groups == 0);
	CHECK(tickets.size() == 2);
	uint32_t resident_bystanders = 0;
	for (uint32_t n : { 4u, 5u, 6u }) { resident_bystanders += res.state(n) == Residency::RESIDENT ? 1u : 0u; }
	CHECK(resident_bystanders == 3);
	CHECK(res.stats().pages_retiring == 0);
	CHECK(res.stats().allocation_no_contiguous_run_total == 0);
}

static bool fragmented_page_pool(Residency &r_res) {
	Vector<Residency::NodeInfo> infos;
	if (infos.resize(11) != OK) { return false; }
	const uint32_t parents[11] = { UINT32_MAX, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
	for (uint32_t i = 0; i < 11; ++i) {
		infos.write[i].splat_count = i == 2 || i == 3 ? 1500 : 1000;
		infos.write[i].parent = parents[i];
		infos.write[i].height = i == 0 ? 2 : (i == 1 ? 1 : 0);
	}
	if (!r_res.reset(infos, config(9, 3), 1, 1)) { return false; }
	// Root and parent, then alternating fillers and bystanders, fill all nine pages.
	uint64_t frame = 1;
	for (uint32_t node : { 0u, 1u, 7u, 4u, 8u, 5u, 9u, 6u, 10u }) {
		if (!make_resident_at(r_res, node, frame++)) { return false; }
	}
	r_res.begin_frame(20);
	r_res.withdraw_transitions();
	for (uint32_t node : { 7u, 8u, 9u, 10u }) {
		if (!r_res.evict(node)) { return false; }
	}
	r_res.begin_frame(23); // Retired fillers leave isolated free pages 2, 4, 6 and 8.
	return true;
}

static bool isolated_free_pages(const Residency &p_res) {
	bool previous_free = false;
	uint32_t free = 0;
	for (uint32_t owner : p_res.page_owners()) {
		const bool current_free = owner == Residency::FREE_PAGE;
		if (current_free && previous_free) { return false; }
		free += current_free ? 1u : 0u;
		previous_free = current_free;
	}
	return free == p_res.stats().pages_free;
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a fragmented pool places a group in the same frame when the free pages suffice (#1514)") {
	Residency res;
	if (!fragmented_page_pool(res)) { FAIL("could not construct the fragmented page pool"); return; }
	const auto before = res.stats();
	if (before.pages_free != 4 || before.largest_free_run != 4 || before.pages_retiring != 0 ||
			res.pages_for(2) != 2 || res.pages_for(3) != 2 || !isolated_free_pages(res)) {
		FAIL("expected four isolated free pages for two two-page siblings"); return;
	}
	LocalVector<Residency::Ticket> tickets;
	const uint32_t group[2] = { 2, 3 };
	const auto result = res.request(group, 2, tickets);
	CHECK(result.requested == 2);
	CHECK(tickets.size() == 2);
	for (uint32_t node : group) {
		uint32_t matching_tickets = 0;
		for (const auto &ticket : tickets) {
			if (ticket.node == node && !ticket.shadow && res.is_pending(ticket)) { ++matching_tickets; }
			CHECK(res.allocation_matches(ticket));
		}
		CHECK(matching_tickets == 1);
	}
	const auto after = res.stats();
	CHECK(result.fragmented_groups == 0);
	CHECK(after.allocation_no_contiguous_run_total - before.allocation_no_contiguous_run_total == 0);
	CHECK(after.evicted_total - before.evicted_total == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a fragmented pool places edit shadows in the same frame when the free pages suffice (#1514)") {
	Residency res;
	if (!fragmented_page_pool(res)) { FAIL("could not construct the fragmented page pool"); return; }
	const auto before = res.stats();
	if (before.pages_free != 4 || before.largest_free_run != 4 || before.pages_retiring != 0 ||
			res.pages_for(2) != 2 || res.pages_for(3) != 2 || !isolated_free_pages(res)) {
		FAIL("expected four isolated free pages for two two-page shadows"); return;
	}
	LocalVector<Residency::Ticket> tickets;
	const Residency::EditedLeaf leaves[2] = { { 2, 1500 }, { 3, 1500 } };
	CHECK(res.stage_edit(1, leaves, 2, tickets) == Residency::EDIT_OK);
	res.advance_edit(tickets);
	CHECK(tickets.size() == 2);
	for (const auto &leaf : leaves) {
		uint32_t matching_tickets = 0;
		for (const auto &ticket : tickets) {
			if (ticket.node == leaf.node && ticket.shadow && res.is_shadow_pending(ticket)) { ++matching_tickets; }
			CHECK(res.allocation_matches(ticket));
		}
		CHECK(matching_tickets == 1);
	}
	const auto after = res.stats();
	CHECK(after.pages_shadow == 4);
	CHECK_FALSE(after.edit_waiting_for_space);
	CHECK(after.evicted_total - before.evicted_total == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] the contiguous-allocation control refuses a fragmented group (#1514)") {
	Residency res;
	if (!fragmented_page_pool(res) || !isolated_free_pages(res)) { FAIL("could not construct isolated free pages"); return; }
	res.test_set_require_contiguous(true);
	const auto before = res.stats();
	LocalVector<Residency::Ticket> tickets;
	const uint32_t group[2] = { 2, 3 };
	const auto result = res.request(group, 2, tickets);
	CHECK(result.requested == 0);
	CHECK(tickets.is_empty());
	CHECK(res.state(2) == Residency::ABSENT);
	CHECK(res.state(3) == Residency::ABSENT);
	CHECK(res.stats().pages_free == before.pages_free);
	CHECK(res.stats().evicted_total == before.evicted_total);
	res.test_set_require_contiguous(false);
	res.withdraw_transitions();
	CHECK(res.request(group, 2, tickets).requested == 2);
	CHECK(tickets.size() == 2);
}

static void check_page_tables(const Residency &p_res, const LocalVector<Residency::Ticket> &p_retiring) {
	const uint32_t capacity = p_res.config().page_capacity;
	const uint32_t width = p_res.max_pages_per_node();
	if (uint64_t(p_res.page_table().size()) != uint64_t(capacity) * width || uint32_t(p_res.page_owners().size()) != capacity) {
		FAIL("page-table buffers do not match atlas capacity"); return;
	}
	Vector<uint32_t> expected;
	Vector<uint8_t> occupied_slots;
	if (expected.resize(capacity) != OK || occupied_slots.resize(capacity) != OK || width == 0) {
		FAIL("could not prepare page-table accounting"); return;
	}
	expected.fill(Residency::FREE_PAGE);
	occupied_slots.fill(0);
	uint32_t pages_owned = 0;
	auto observe = [&](uint32_t p_node, const Vector<uint32_t> &p_pages, uint32_t p_base) {
		if (p_pages.is_empty() || p_base % width != 0 || p_base / width >= capacity ||
				uint64_t(p_base) + p_pages.size() > uint64_t(p_res.page_table().size())) {
			FAIL("invalid owned page-table slot"); return;
		}
		CHECK(occupied_slots[p_base / width] == 0);
		occupied_slots.write[p_base / width] = 1;
		for (uint32_t k = 0; k < uint32_t(p_pages.size()); ++k) {
			const uint32_t page = p_pages[k];
			if (page >= capacity) { FAIL("owned page exceeds atlas capacity"); return; }
			CHECK(expected[page] == Residency::FREE_PAGE);
			expected.write[page] = p_node;
			CHECK(p_res.page_table()[p_base + k] == page);
			++pages_owned;
		}
	};
	for (uint32_t node = 0; node < p_res.node_count(); ++node) {
		Vector<uint32_t> pages;
		uint32_t base = 0;
		if (p_res.page_allocation(node, pages, base)) { observe(node, pages, base); }
		Residency::Ticket shadow;
		if (p_res.shadow_ticket(node, shadow)) {
			CHECK(p_res.allocation_matches(shadow));
			observe(node, shadow.pages, shadow.pt_base);
		}
	}
	for (const auto &ticket : p_retiring) { observe(ticket.node, ticket.pages, ticket.pt_base); }
	CHECK(p_res.page_owners() == expected);
	for (uint32_t slot = 0; slot < capacity; ++slot) {
		if (!occupied_slots[slot]) {
			for (uint32_t k = 0; k < width; ++k) { CHECK(p_res.page_table()[slot * width + k] == Residency::FREE_PAGE); }
		}
	}
	const auto stats = p_res.stats();
	CHECK(pages_owned == stats.pages_resident + stats.pages_in_flight + stats.pages_retiring);
	CHECK(stats.pages_free + pages_owned == capacity);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] forward and reverse tables retain ownership through evict retire and readmit (#1514)") {
	Residency res;
	if (!fragmented_page_pool(res)) { FAIL("could not construct the fragmented page pool"); return; }
	LocalVector<Residency::Ticket> retired;
	check_page_tables(res, retired);
	LocalVector<Residency::Ticket> tickets;
	const uint32_t group[2] = { 2, 3 };
	res.request(group, 2, tickets);
	if (tickets.size() != 2) { FAIL("expected the whole two-node refinement group"); return; }
	for (const auto &ticket : tickets) {
		CHECK(res.allocation_matches(ticket));
		if (!res.begin_upload(ticket) || !res.finish_upload(ticket, true)) { FAIL("could not complete page-list payload"); return; }
	}
	check_page_tables(res, retired);
	for (uint64_t frame : { 24u, 28u, 32u }) {
		res.begin_frame(frame);
		res.withdraw_transitions();
		Residency::Ticket old;
		for (const auto &ticket : tickets) { if (ticket.node == 2) { old = ticket; } }
		if (!old.valid() || !res.evict(2)) { FAIL("could not evict the page-list node"); return; }
		retired.push_back(old);
		CHECK(res.stats().pages_retiring == 2);
		check_page_tables(res, retired);
		res.begin_frame(frame + 2);
		check_page_tables(res, retired);
		res.begin_frame(frame + 3);
		retired.clear();
		CHECK(res.stats().pages_retiring == 0);
		check_page_tables(res, retired);
		tickets.clear();
		const uint32_t node = 2;
		res.request(&node, 1, tickets);
		if (tickets.size() != 1) { FAIL("retired pages did not admit their node again"); return; }
		CHECK(res.allocation_matches(tickets[0]));
		CHECK_FALSE(res.allocation_matches(old));
		if (!res.begin_upload(tickets[0]) || !res.finish_upload(tickets[0], true)) { FAIL("readmitted node could not become resident"); return; }
		check_page_tables(res, retired);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] edit commit swaps page lists and retains old forward slots until retirement (#1514)") {
	Residency res;
	Vector<Residency::NodeInfo> infos;
	if (infos.resize(3) != OK) { FAIL("could not prepare edit nodes"); return; }
	for (uint32_t i = 0; i < 3; ++i) {
		infos.write[i].parent = i == 0 ? UINT32_MAX : 0;
		infos.write[i].splat_count = i == 0 ? 1000 : 1500;
	}
	if (!res.reset(infos, config(10, 3), 1, 1)) { FAIL("could not initialize edit pool"); return; }
	for (uint32_t node : { 0u, 1u, 2u }) {
		if (!make_resident_at(res, node, node + 1)) { FAIL("could not populate old edit payloads"); return; }
	}
	res.begin_frame(10);
	res.withdraw_transitions();
	LocalVector<Residency::Ticket> retired;
	for (uint32_t node : { 0u, 1u }) {
		Residency::Ticket old;
		old.node = node;
		if (!res.page_allocation(node, old.pages, old.pt_base)) { FAIL("old edit allocation is missing"); return; }
		retired.push_back(old);
	}
	LocalVector<Residency::Ticket> shadows;
	const Residency::EditedLeaf leaf = { 1, 1500 };
	if (res.stage_edit(1, &leaf, 1, shadows) != Residency::EDIT_OK || shadows.size() != 1) {
		FAIL("could not reserve the edit shadow"); return;
	}
	LocalVector<Residency::Ticket> none;
	check_page_tables(res, none);
	const auto shadow = shadows[0];
	CHECK(shadow.pt_base != retired[1].pt_base);
	CHECK(shadow.pages != retired[1].pages);
	if (!res.finish_shadow(shadow, true) || !res.finish_shadow_transfer(shadow) || !res.commit_edit()) {
		FAIL("transferred shadow did not commit"); return;
	}
	Vector<uint32_t> committed_pages;
	uint32_t committed_base = 0;
	if (!res.page_allocation(1, committed_pages, committed_base)) { FAIL("committed edit lost its allocation"); return; }
	CHECK(committed_pages == shadow.pages);
	CHECK(committed_base == shadow.pt_base);
	CHECK(res.state(1) == Residency::RESIDENT);
	CHECK(res.stats().pages_retiring == 3);
	check_page_tables(res, retired);
	res.begin_frame(12);
	check_page_tables(res, retired);
	res.begin_frame(13);
	CHECK(res.stats().pages_retiring == 0);
	check_page_tables(res, none);
}

}
