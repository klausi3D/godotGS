extern "C" int test_hlod_node_residency_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../lod/hlod_node_residency.h"
#include "../lod/hlod_runtime_selection.h"
#include "../renderer/hlod_node_streamer.h"
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

TEST_CASE("[GaussianSplatting][HLOD][Residency] pressure telemetry distinguishes measured zero, fullness and fragmentation") {
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
		// are three free pages, split by protected node 4. Node 3 needs three contiguous pages.
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
		CHECK(result.skipped_groups == 1);
		CHECK(result.fragmented_groups == 1);
		CHECK(result.evicted == 0);
		CHECK(tickets.size() == 0);
		const auto stats = res.stats();
		CHECK(stats.pressure_measured);
		CHECK(stats.pages_free == 3);
		CHECK(stats.largest_free_run == 2);
		CHECK(stats.allocation_refused_total == 1);
		CHECK(stats.allocation_no_contiguous_run_total == 1);
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

TEST_CASE("[GaussianSplatting][HLOD][Residency] irreducible drawn fragmentation retains complete coverage and bounds unchanged retries (#1483 B)") {
	Sim sim;
	sim.data.instantiate();
	binary_tree(*sim.data.ptr(), 3, 1000, 1000);
	sim.data->nodes.write[3].topology.payload_count = 2500; // Three contiguous pages.
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
	CHECK(sim.res.stats().largest_free_run == 2);
	sim.frame = 20;
	sim.req.page_splats = 1024;
	sim.req.page_budget = 7; // The whole desired closure fits; its three-page run cannot be placed.
	sim.step();
	if (sim.demand != std::vector<uint32_t>({ 3, 4 })) {
		FAIL("fragmentation must leave the highest-priority transition as unmet selection demand"); return;
	}
	CHECK(sim.request_result.fragmented_groups == 1);
	CHECK(sim.tickets.is_empty());
	const auto blocked = sim.res.stats();
	CHECK(blocked.allocation_refused_total > 0);
	CHECK(blocked.transition_state == Residency::COARSE_PLACEMENT_BLOCKED);
	// With no residency/cap/owner change, coarse blockage must suppress further placement churn.
	for (uint32_t retry = 0; retry < 12; ++retry) {
		sim.step();
		CHECK(sim.cut.status == CutHistory::COMPLETE);
		CHECK(every_drawn_resident(sim));
		CHECK(exact_cover(sim, 3));
		CHECK(sim.demand.size() > 0); // Empty executable work must never masquerade as satisfied quality.
		CHECK(sim.tickets.is_empty());
		CHECK(sim.res.stats().pages_in_flight == 0);
		CHECK(sim.res.stats().evicted_total == blocked.evicted_total);
		CHECK(sim.res.stats().requested_total == blocked.requested_total);
		CHECK(sim.res.stats().transition_state == Residency::COARSE_PLACEMENT_BLOCKED);
	}
	CHECK(sim.res.stats().allocation_refused_total == blocked.allocation_refused_total);
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
	std::vector<uint32_t> reserved_pages(count);
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t pages = 0;
		if (!res.run(frontier[i], reserved_pages[i], pages)) {
			FAIL("bootstrap did not reserve every run before starting its first request batch"); return;
		}
		CHECK(pages == 1);
		CHECK(res.is_protected(frontier[i]));
	}
	CHECK(res.stats().pages_free == 0); // The eight deferred requests already own reserved runs.
	for (uint32_t i = 0; i < cap; ++i) {
		CHECK(first[i].node == frontier[i]);
		CHECK(first[i].first_page == reserved_pages[i]);
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
		CHECK(last[i].first_page == reserved_pages[cap + i]); // No second allocation or run movement.
		CHECK(last[i].page_count == 1);
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
			CHECK(ticket.first_page == i + 1); // Fresh one-page atlas: root occupies page zero.
			CHECK(ticket.page_count == 1);
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

TEST_CASE("[GaussianSplatting][HLOD][Residency] a failed child is reported as such, and a fragmentation stall ends by evicting an unprotected neighbour") {
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
	// Fragmented: pages 1-2 and 4 are free (3 in total, largest run 2); node 3 needs 3 contiguous pages.
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
		CHECK(res.stats().largest_free_run == 2);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t want[1] = { 3 };
		const auto result = res.request(want, 1, tickets);
		// The stall ends: the unprotected neighbour is evicted and the 3 pages form one run.
		CHECK(result.evicted == 1);
		CHECK(result.requested == 1);
		CHECK(res.state(4) == Residency::ABSENT);
		CHECK(res.state(3) == Residency::LOADING);
		CHECK(res.state(0) == Residency::RESIDENT);
	}
	// Same atlas, but the neighbour is protected (drawn): no window exists, nothing is evicted, and the stall
	// is reported as fragmentation, not as a budget or a failure.
	{
		Residency res;
		if (!fragmented_atlas(res)) { FAIL("fragmented atlas setup failed"); return; }
		const uint32_t drawn[1] = { 4 };
		res.protect_drawn(drawn, 1);
		LocalVector<Residency::Ticket> tickets;
		const uint32_t want[1] = { 3 };
		const auto result = res.request(want, 1, tickets);
		CHECK(result.skipped_groups == 1);
		CHECK(result.fragmented_groups == 1);
		CHECK(result.blocked_groups == 0);
		CHECK(result.evicted == 0);
		CHECK(tickets.size() == 0);
		CHECK(res.state(4) == Residency::RESIDENT);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] at the production retire delay a fragmentation stall evicts one window, and a retiring shortfall evicts nothing") {
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
	res.begin_frame(23); // The fillers retired: p1, p3, p5, p7 are free.
	CHECK(res.stats().pages_free == 4);
	CHECK(res.stats().largest_free_run == 1);
	LocalVector<Residency::Ticket> tickets;
	const uint32_t group[2] = { 2, 3 }; // 4 pages in two 2-page runs.
	auto result = res.request(group, 2, tickets);
	// One window is evicted (its pages retire for 3 frames, so nothing can be placed yet); the other bystanders
	// stay resident until that window has retired.
	CHECK(result.evicted == 1);
	CHECK(result.requested == 0);
	uint32_t resident_bystanders = 0;
	for (uint32_t n : { 4u, 5u, 6u }) { resident_bystanders += res.state(n) == Residency::RESIDENT ? 1u : 0u; }
	CHECK(resident_bystanders == 2);
	// The next frames: the retiring window is awaited, no further bystander is evicted.
	res.begin_frame(24);
	result = res.request(group, 2, tickets);
	CHECK(result.evicted == 0);
	res.begin_frame(25);
	result = res.request(group, 2, tickets);
	CHECK(result.evicted == 0);
	// Once the first window retired it holds one 2-page run, but both runs are needed (all or none): the second
	// window is evicted now, one at a time, and the group is placed when that one has retired too.
	res.begin_frame(26);
	result = res.request(group, 2, tickets);
	CHECK(result.requested == 0);
	CHECK(result.evicted == 1);
	res.begin_frame(27);
	result = res.request(group, 2, tickets);
	CHECK(result.evicted == 0);
	res.begin_frame(29);
	result = res.request(group, 2, tickets);
	CHECK(result.requested == 2);
	CHECK(result.evicted == 0);
	CHECK(res.state(6) == Residency::RESIDENT); // The third bystander was never needed.
}

// Synchronous CPU device sink: allocation, cached B blockage, capped copying, the safe frame-boundary
// switch and retirement all use production paths. There are no workers or asynchronous readiness polls.
struct CompactionFixture {
	using Streamer = HlodNodeStreamer;
	static constexpr uint64_t FIRST_BLOCKED_FRAME = 100;
	static constexpr uint64_t FIRST_COPY_FRAME = FIRST_BLOCKED_FRAME + 7; // The required R=8, independent of the red-proof constant.
	static constexpr uint32_t SOURCE = 2, BLOCKED = 7, COMPETITOR = 8;
	Ref<GaussianSplatHlodRuntimeData> data;
	Streamer streamer;
	Streamer::Buffers buffers;
	LocalVector<Residency::Ticket> tickets;
	uint32_t source_pages = 2, hole_pages = 3, target_pages = 8, capacity = 14;
	uint32_t source_first = 4, destination_first = 10;
	uint32_t drawn[4] = { 0, 4, 6, SOURCE };
	uint32_t drawn_count = 3;

	Residency &residency() { return streamer._test_residency(); }

	bool init(uint32_t p_source_pages = 2, bool p_quantized = false) {
		source_pages = p_source_pages;
		hole_pages = source_pages + 1;
		target_pages = 2 * hole_pages + source_pages;
		capacity = 2 * hole_pages + 2 * source_pages + 4;
		source_first = 1 + hole_pages;
		destination_first = 2 * hole_pages + source_pages + 2;
		data.instantiate();
		if (data->nodes.resize(9) != OK) { FAIL("could not size compaction node fixture"); return false; }
		const uint32_t pages[9] = { 1, hole_pages, source_pages, hole_pages, 1, source_pages, 1, target_pages, target_pages };
		for (uint32_t node = 0; node < 9; ++node) {
			auto &topology = data->nodes.write[node].topology;
			topology.payload_count = pages[node] * 1024;
			// The competitor is a distinct transition. Adding it to BLOCKED's sibling group would
			// legitimately make that whole group exceed this fixture's page budget.
			topology.parent = node == 0 ? gs_hlod::kNoParent : (node == BLOCKED ? SOURCE : (node == COMPETITOR ? 4 : 0));
			topology.height = node == 0 ? 2 : (node == SOURCE ? 1 : 0);
		}
		Streamer::Config cfg;
		cfg.atlas_pages = capacity;
		cfg.retire_delay_frames = 3;
		cfg.quantized = p_quantized;
		if (!streamer.init(data, cfg, 1, 1)) { FAIL("compaction streamer initialization failed"); return false; }
		Streamer::TestControl control;
		control.cpu_upload_sink = true;
		if (streamer._test_set_control(control) != OK || streamer._test_observe_shadow_transfers() != OK) {
			FAIL("compaction fixture requires the real CPU device-write observations"); return false;
		}
		// For S=2: root@0, filler@1-3, source@4-5, filler@6-8, drawn@9,
		// destination filler@10-11, drawn@12, scratch@13. The exact one-page scratch
		// hole keeps its budget charge out of the target hole; the exact S-page destination is elsewhere.
		for (uint32_t node = 0; node < 7; ++node) {
			if (!make_resident_at(residency(), node, node + 1) || streamer._test_seed_resident_metadata(node) != OK) {
				FAIL("could not populate compaction fixture runs and metadata"); return false;
			}
		}
		residency().begin_frame(20);
		residency().request(nullptr, 0, tickets); // Withdraw fixture-only leases before removing fillers.
		if (!residency().evict(1) || !residency().evict(3) || !residency().evict(5)) {
			FAIL("could not fragment compaction fixture"); return false;
		}
		residency().begin_frame(23);
		if (residency().stats().pages_free != 2 * hole_pages + source_pages + 1 ||
				residency().stats().largest_free_run != hole_pages) {
			FAIL("fixture must have enough total free pages without a fitting run"); return false;
		}
		return true;
	}

	void begin(uint64_t p_token, bool p_source_drawn = false) {
		drawn_count = p_source_drawn ? 4 : 3;
		streamer.begin_server_frame(p_token, nullptr, &buffers, drawn, drawn_count);
		streamer.begin_frame(nullptr, &buffers, p_token);
		residency().protect_drawn(drawn, drawn_count);
	}

	Residency::RequestResult request(bool p_competitor = false) {
		tickets.clear();
		const uint32_t demand[2] = { BLOCKED, COMPETITOR };
		return residency().request(demand, p_competitor ? 2 : 1, tickets);
	}

	Residency::RequestResult step(uint64_t p_token, bool p_source_drawn = false, bool p_competitor = false) {
		begin(p_token, p_source_drawn);
		const auto result = request(p_competitor);
		streamer._test_compaction_step(nullptr, buffers, drawn, drawn_count);
		return result;
	}

	bool reach_copy() {
		for (uint64_t token = FIRST_BLOCKED_FRAME; token <= FIRST_COPY_FRAME; ++token) { step(token); }
		if (residency().stats().compaction_pages_moved_total == 0) {
			FAIL("the eighth consecutive real blocked demand must start compaction"); return false;
		}
		return true;
	}

	bool run_is(uint32_t p_node, uint32_t p_first, uint32_t p_pages) {
		uint32_t first = 0, pages = 0;
		return residency().run(p_node, first, pages) && first == p_first && pages == p_pages;
	}

	bool metadata_base_is(uint32_t p_node, uint32_t p_first) const {
		ChunkMetaGPU meta = {};
		return streamer._test_compaction_meta(p_node, meta) && meta.atlas_base == p_first * 1024;
	}

	void check_accounting() {
		const auto stats = residency().stats();
		CHECK(stats.pages_resident + stats.pages_in_flight + stats.pages_retiring == residency().allocator_used_pages());
		CHECK(residency().allocator_used_pages() + stats.pages_free == capacity);
	}
};

TEST_CASE("[GaussianSplatting][HLOD][Residency] cached coarse blockage starts undrawn compaction on server frame eight and admits the node (#1483 C)") {
	CompactionFixture h;
	if (!h.init()) { return; }
	uint64_t refused = 0;
	for (uint64_t token = h.FIRST_BLOCKED_FRAME; token < h.FIRST_COPY_FRAME; ++token) {
		const auto result = h.step(token);
		CHECK(result.requested == 0);
		CHECK(h.tickets.is_empty());
		CHECK_FALSE(h.residency().compaction_active());
		CHECK(h.residency().stats().compaction_pages_moved_total == 0);
		if (token == h.FIRST_BLOCKED_FRAME) { refused = h.residency().stats().allocation_refused_total; }
		CHECK(h.residency().stats().allocation_refused_total == refused); // B's cached coarse_blocked branch.
	}
	CHECK(refused > 0);
	h.step(h.FIRST_COPY_FRAME);
	// The charged duplicate temporarily makes new group planning budget-bound; the already-started
	// move must remain needed and complete without treating its own reservation as withdrawal.
	CHECK(h.residency().compaction_move_needed());
	CHECK_FALSE(h.residency().compaction_active());
	CHECK(h.residency().stats().compaction_pages_moved_total == h.source_pages);
	CHECK(h.residency().compaction_move().node == h.SOURCE);
	CHECK(h.run_is(h.SOURCE, h.source_first, h.source_pages));
	CHECK(h.metadata_base_is(h.SOURCE, h.source_first));
	CHECK(h.residency().stats().compaction_moves_total == 0); // Submitted pages are not GPU completion.
	// Three server frames to complete the copy, then three to retire its old source.
	for (uint64_t token = h.FIRST_COPY_FRAME + 1; token <= h.FIRST_COPY_FRAME + 6; ++token) { h.step(token); }
	CHECK(h.residency().stats().compaction_moves_total == 1);
	if (h.tickets.size() != 1) { FAIL("compaction must make the blocked whole run placeable"); return; }
	CHECK(h.tickets[0].node == h.BLOCKED);
	CHECK(h.residency().begin_upload(h.tickets[0]));
	CHECK(h.residency().finish_upload(h.tickets[0], true));
	CHECK(h.residency().state(h.BLOCKED) == Residency::RESIDENT);
	h.check_accounting();
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a drawn node never moves during useful undrawn compaction (#1483 C)") {
	CompactionFixture h;
	if (!h.init()) { return; }
	const uint32_t drawn_first = 2 * h.hole_pages + h.source_pages + 1;
	bool unchanged_before_copy = true;
	for (uint64_t token = h.FIRST_BLOCKED_FRAME; token <= h.FIRST_COPY_FRAME; ++token) {
		h.step(token);
		unchanged_before_copy = h.run_is(4, drawn_first, 1) && h.metadata_base_is(4, drawn_first) && unchanged_before_copy;
	}
	CHECK(h.residency().stats().compaction_pages_moved_total > 0); // A disabled feature cannot satisfy safety vacuously.
	CHECK(unchanged_before_copy);
	for (uint64_t token = h.FIRST_COPY_FRAME; token <= h.FIRST_COPY_FRAME + 6; ++token) {
		if (token != h.FIRST_COPY_FRAME) { h.step(token); }
		CHECK(h.run_is(4, drawn_first, 1));
		CHECK(h.metadata_base_is(4, drawn_first));
		CHECK(h.residency().state(4) == Residency::RESIDENT);
	}
	CHECK(h.residency().stats().compaction_moves_total > 0);
	const auto copies = h.streamer._test_compaction_copies();
	CHECK_FALSE(copies.is_empty());
	for (const auto &copy : copies) {
		CHECK(copy.node == h.SOURCE);
		CHECK(copy.node != 0);
		CHECK(copy.node != 4);
		CHECK(copy.node != 6);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] completed compaction defers a drawn switch and retires its authoritative source (#1483 C)") {
	CompactionFixture h;
	if (!h.init(2, true) || !h.reach_copy()) { return; }
	CHECK(h.residency().stats().compaction_pages_moved_total > 0);
	for (uint64_t token = h.FIRST_COPY_FRAME + 1; token <= h.FIRST_COPY_FRAME + 3; ++token) {
		h.step(token, true); // The source became drawn after submission; it keeps using its original address.
		CHECK(h.run_is(h.SOURCE, h.source_first, h.source_pages));
		CHECK(h.metadata_base_is(h.SOURCE, h.source_first));
		CHECK(h.residency().stats().compaction_moves_total == 0);
	}
	CHECK(h.residency().compaction_copy_complete());
	CHECK(h.residency().stats().compaction_switch_deferred_total == 1);
	h.streamer.begin_frame(nullptr, &h.buffers, h.FIRST_COPY_FRAME + 3); // Another view of the same server frame.
	CHECK(h.residency().stats().compaction_switch_deferred_total == 1);
	const uint64_t switch_boundary_view_frame = h.residency().frame();
	h.step(h.FIRST_COPY_FRAME + 4);
	CHECK(h.residency().stats().compaction_moves_total == 1);
	CHECK(h.run_is(h.SOURCE, h.destination_first, h.source_pages));
	CHECK(h.metadata_base_is(h.SOURCE, h.destination_first));
	CHECK(h.residency().stats().pages_retiring == h.source_pages);
	const uint32_t charged = h.residency().allocator_used_pages();
	for (uint64_t token = h.FIRST_COPY_FRAME + 5; token < h.FIRST_COPY_FRAME + 7; ++token) {
		h.step(token);
		CHECK(h.residency().allocator_used_pages() == charged);
		CHECK(h.residency().stats().pages_retiring == h.source_pages);
	}
	h.begin(h.FIRST_COPY_FRAME + 7);
	// The old source retires now, and compaction ends, releasing its already-retired scratch page too.
	CHECK(h.residency().allocator_used_pages() == charged - h.source_pages - 1);
	CHECK(h.residency().stats().pages_retiring == 0);
	CHECK_FALSE(h.residency().has_compaction_scratch());
	h.check_accounting();
	// Inspect the actual metadata/quantization write arguments: both address words switch together.
	Vector<CompactionFixture::Streamer::TestWrite> writes;
	if (h.streamer._test_shadow_writes(writes) != OK) { FAIL("compaction address-write observations unavailable"); return; }
	uint32_t meta_switches = 0, quant_switches = 0;
	for (const auto &write : writes) {
		if (write.node != h.SOURCE || write.bytes != sizeof(uint32_t)) { continue; }
		if (write.kind == CompactionFixture::Streamer::TestWrite::METADATA) {
			++meta_switches;
			CHECK(write.meta.atlas_base == h.destination_first * 1024);
			CHECK(write.frame == switch_boundary_view_frame);
		} else if (write.kind == CompactionFixture::Streamer::TestWrite::QUANTIZATION) {
			++quant_switches;
			CHECK(write.quant.start_index == h.destination_first * 1024);
			CHECK(write.frame == switch_boundary_view_frame);
		}
	}
	CHECK(meta_switches == 1);
	CHECK(quant_switches == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] compaction charges scratch and both runs until source retirement (#1483 C)") {
	CompactionFixture h;
	if (!h.init() || !h.reach_copy()) { return; }
	const uint32_t resident_pages = h.source_pages + 3;
	const uint32_t dual_charge = resident_pages + h.source_pages + 1;
	CHECK(h.residency().stats().compaction_pages_moved_total > 0);
	CHECK(h.residency().has_compaction_scratch());
	CHECK(h.residency().allocator_used_pages() == dual_charge);
	CHECK(h.residency().stats().pages_resident == resident_pages);
	CHECK(h.residency().stats().pages_in_flight == h.source_pages + 1);
	h.check_accounting();
	for (uint64_t token = h.FIRST_COPY_FRAME + 1; token < h.FIRST_COPY_FRAME + 6; ++token) {
		h.step(token, false, true); // Offer a competing whole run while preserving the original blocked demand.
		CHECK(h.residency().state(h.COMPETITOR) == Residency::ABSENT);
		CHECK(h.tickets.is_empty());
		CHECK(h.residency().allocator_used_pages() == dual_charge);
		h.check_accounting();
		if (token >= h.FIRST_COPY_FRAME + 3) {
			CHECK(h.residency().stats().compaction_moves_total == 1);
			CHECK(h.residency().stats().pages_retiring == h.source_pages);
			CHECK(h.residency().stats().pages_in_flight == 1); // Only scratch remains in flight after the switch.
		}
	}
	h.begin(h.FIRST_COPY_FRAME + 6);
	CHECK(h.residency().stats().compaction_moves_total == 1);
	CHECK(h.residency().allocator_used_pages() == dual_charge - h.source_pages - 1);
	CHECK(h.residency().stats().pages_retiring == 0);
	CHECK_FALSE(h.residency().has_compaction_scratch());
	h.check_accounting();
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] compaction page cap is shared by all views of one server frame (#1483 C)") {
	CompactionFixture h;
	if (!h.init(6) || !h.reach_copy()) { return; }
	CHECK(h.residency().stats().compaction_pages_moved_total == 4);
	CHECK(h.residency().stats().compaction_frames_total == 1);
	CHECK(h.residency().stats().compaction_cap_limited_frames_total == 1);
	CHECK(h.residency().compaction_move().copied_pages == 4);
	for (uint32_t view = 0; view < 3; ++view) {
		h.streamer.begin_frame(nullptr, &h.buffers, h.FIRST_COPY_FRAME);
		h.residency().protect_drawn(h.drawn, h.drawn_count);
		h.request();
		h.streamer._test_compaction_step(nullptr, h.buffers, h.drawn, h.drawn_count);
		CHECK(h.residency().stats().compaction_pages_moved_total == 4);
		CHECK(h.residency().stats().compaction_frames_total == 1);
		CHECK(h.residency().compaction_move().copied_pages == 4);
	}
	h.step(h.FIRST_COPY_FRAME + 1);
	CHECK(h.residency().stats().compaction_pages_moved_total == 6);
	CHECK(h.residency().stats().compaction_frames_total == 2);
	CHECK(h.residency().stats().compaction_cap_limited_frames_total == 1);
	CHECK(h.residency().stats().compaction_moves_total == 0);
	CHECK(h.run_is(h.SOURCE, h.source_first, h.source_pages));
	const auto copies = h.streamer._test_compaction_copies();
	if (copies.size() != 6) { FAIL("six source pages must produce six actual page-copy observations"); return; }
	const uint64_t page_bytes = 1024 * sizeof(PackedGaussian);
	for (uint32_t page = 0; page < 6; ++page) {
		CHECK(copies[page].server_frame == h.FIRST_COPY_FRAME + (page < 4 ? 0 : 1));
		CHECK(copies[page].node == h.SOURCE);
		CHECK(copies[page].source_offset == uint64_t(h.source_first + page) * page_bytes);
		CHECK(copies[page].destination_offset == uint64_t(h.destination_first + page) * page_bytes);
		CHECK(copies[page].bytes == page_bytes);
	}
	h.step(h.FIRST_COPY_FRAME + 2);
	h.step(h.FIRST_COPY_FRAME + 3);
	CHECK(h.residency().stats().compaction_moves_total == 0);
	h.step(h.FIRST_COPY_FRAME + 4); // Completion delay is counted from the last copied page, not the first.
	CHECK(h.residency().stats().compaction_moves_total == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] active compaction counts frames with every resident source drawn (#1483 C)") {
	CompactionFixture h;
	if (!h.init()) { return; }
	for (uint64_t token = h.FIRST_BLOCKED_FRAME; token <= h.FIRST_COPY_FRAME; ++token) { h.step(token, true); }
	CHECK(h.residency().compaction_active()); // Threshold-disabled red proof must fail here, too.
	CHECK(h.residency().stats().compaction_no_candidate_frames_total == 1);
	CHECK(h.residency().stats().compaction_pages_moved_total == 0);
	CHECK(h.residency().stats().compaction_frames_total == 0);
	CHECK(h.residency().stats().compaction_cap_limited_frames_total == 0);
	CHECK(h.residency().stats().compaction_moves_total == 0);
	CHECK(h.run_is(h.SOURCE, h.source_first, h.source_pages));
	h.streamer.begin_frame(nullptr, &h.buffers, h.FIRST_COPY_FRAME);
	h.streamer._test_compaction_step(nullptr, h.buffers, h.drawn, h.drawn_count);
	CHECK(h.residency().stats().compaction_no_candidate_frames_total == 1);
	h.step(h.FIRST_COPY_FRAME + 1, true);
	CHECK(h.residency().stats().compaction_no_candidate_frames_total == 2);
	CHECK(h.residency().stats().compaction_frames_total == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] withdrawn demand and changed transition identity restart compaction's blocked streak (#1483 C)") {
	SUBCASE("withdrawal") {
		CompactionFixture h;
		if (!h.init()) { return; }
		for (uint64_t token = h.FIRST_BLOCKED_FRAME; token < h.FIRST_BLOCKED_FRAME + 4; ++token) { h.step(token); }
		for (uint64_t token = h.FIRST_BLOCKED_FRAME + 4; token < h.FIRST_BLOCKED_FRAME + 6; ++token) {
			h.begin(token);
			h.tickets.clear();
			h.residency().request(nullptr, 0, h.tickets);
			h.streamer._test_compaction_step(nullptr, h.buffers, h.drawn, h.drawn_count);
			CHECK_FALSE(h.residency().compaction_active());
		}
		for (uint64_t token = h.FIRST_BLOCKED_FRAME + 6; token < h.FIRST_BLOCKED_FRAME + 13; ++token) {
			h.step(token);
			CHECK(h.residency().stats().compaction_pages_moved_total == 0);
		}
		h.step(h.FIRST_BLOCKED_FRAME + 13);
		CHECK(h.residency().stats().compaction_pages_moved_total == h.source_pages);
	}
	SUBCASE("transition identity") {
		CompactionFixture h;
		if (!h.init()) { return; }
		for (uint64_t token = h.FIRST_BLOCKED_FRAME; token < h.FIRST_COPY_FRAME; ++token) { h.step(token); }
		TransitionGroup descriptor;
		descriptor.kind = TRANSITION_REFINEMENT;
		descriptor.parent = h.SOURCE;
		descriptor.first_child = h.BLOCKED;
		descriptor.visible_mask = 1;
		const uint32_t demand = h.BLOCKED;
		for (uint64_t token = h.FIRST_COPY_FRAME; token <= h.FIRST_COPY_FRAME + 7; ++token) {
			h.begin(token);
			h.tickets.clear();
			h.residency().request(&demand, 1, h.tickets, &descriptor);
			h.streamer._test_compaction_step(nullptr, h.buffers, h.drawn, h.drawn_count);
			if (token < h.FIRST_COPY_FRAME + 7) {
				CHECK_FALSE(h.residency().compaction_active());
				CHECK(h.residency().stats().compaction_pages_moved_total == 0);
			}
		}
		CHECK(h.residency().stats().compaction_pages_moved_total == h.source_pages);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] evicting a copied compaction source retires both runs without publishing (#1483 C)") {
	CompactionFixture h;
	if (!h.init() || !h.reach_copy()) { return; }
	CHECK(h.residency().stats().compaction_pages_moved_total > 0);
	const uint32_t charged = h.residency().allocator_used_pages();
	if (!h.residency().evict(h.SOURCE)) { FAIL("undrawn source must be evictable to exercise cancellation"); return; }
	CHECK_FALSE(h.residency().compaction_move().valid());
	CHECK(h.residency().stats().compaction_moves_total == 0);
	CHECK(h.residency().stats().pages_retiring == 2 * h.source_pages);
	CHECK(h.residency().allocator_used_pages() == charged);
	for (uint32_t view = 0; view < 4; ++view) {
		h.streamer.begin_frame(nullptr, &h.buffers, h.FIRST_COPY_FRAME);
		CHECK(h.residency().stats().pages_retiring == 2 * h.source_pages);
		CHECK(h.residency().allocator_used_pages() == charged);
	}
	for (uint64_t token = h.FIRST_COPY_FRAME + 1; token <= h.FIRST_COPY_FRAME + 3; ++token) {
		h.begin(token);
		h.tickets.clear();
		h.residency().request(nullptr, 0, h.tickets);
		h.streamer._test_compaction_step(nullptr, h.buffers, h.drawn, h.drawn_count);
		CHECK(h.residency().stats().compaction_moves_total == 0);
		CHECK(h.residency().state(h.SOURCE) == Residency::ABSENT);
		if (token < h.FIRST_COPY_FRAME + 3) { CHECK(h.residency().allocator_used_pages() == charged); }
	}
	CHECK(h.residency().stats().pages_retiring == 0);
	CHECK_FALSE(h.residency().has_compaction_scratch());
	CHECK(h.residency().allocator_used_pages() == 3); // Only the drawn root and two boundary nodes survive.
	h.check_accounting();
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] whole-group atlas placement simulates supplied-order best-fit without mutation (#1483 C)") {
	GaussianAtlasAllocator allocator;
	allocator.reset(47);
	const uint32_t sizes[8] = { 1, 20, 1, 10, 6, 1, 6, 1 };
	for (uint32_t node = 0; node < 8; ++node) {
		uint32_t first = 0;
		if (!allocator.allocate_slot(node + 1, sizes[node], first)) { FAIL("could not populate whole-group allocator fixture"); return; }
	}
	for (uint64_t key : { 2u, 4u, 7u }) { allocator.release_slot(key); }
	CHECK(allocator.get_free_page_count() == 37);
	CHECK(allocator.get_largest_free_run() == 20);
	CHECK(allocator.can_allocate(16));
	LocalVector<GaussianAtlasAllocator::PageRun> before;
	for (uint32_t index = 0; index < allocator.get_free_run_count(); ++index) {
		GaussianAtlasAllocator::PageRun run;
		if (!allocator.get_free_run(index, run)) { FAIL("could not snapshot allocator holes"); return; }
		before.push_back(run);
	}
	const uint32_t blocked[2] = { 16, 16 };
	uint32_t failed_pages = 0;
	uint64_t placed_pages = 0;
	CHECK_FALSE(allocator.can_allocate_all(blocked, 2, &failed_pages, &placed_pages));
	CHECK(failed_pages == 16);
	CHECK(placed_pages == 16); // Both nodes fit individually, but the second lacks a run after the first.
	// The supplied sequence fails: 4 consumes the six-page hole before either 6.
	// The allocator dry run must preserve the caller's admission order, even when another order fits.
	const uint32_t ordered_failure[5] = { 20, 4, 6, 6, 1 };
	CHECK_FALSE(allocator.can_allocate_all(ordered_failure, 5, &failed_pages, &placed_pages));
	CHECK(failed_pages == 6);
	CHECK(placed_pages == 30);
	CHECK(ordered_failure[0] == 20);
	CHECK(ordered_failure[1] == 4);
	CHECK(ordered_failure[2] == 6);
	CHECK(ordered_failure[3] == 6);
	CHECK(ordered_failure[4] == 1);
	const uint32_t fitting[5] = { 20, 6, 6, 4, 1 };
	CHECK(allocator.can_allocate_all(fitting, 5, &failed_pages, &placed_pages));
	CHECK(failed_pages == 0);
	CHECK(placed_pages == 37);
	CHECK(fitting[0] == 20);
	CHECK(fitting[1] == 6);
	CHECK(fitting[2] == 6);
	CHECK(fitting[3] == 4);
	CHECK(fitting[4] == 1);
	CHECK(allocator.can_allocate_all(nullptr, 0, &failed_pages, &placed_pages));
	CHECK(failed_pages == 0);
	CHECK(placed_pages == 0);
	CHECK(allocator.get_free_page_count() == 37);
	CHECK(allocator.get_used_page_count() == 10);
	CHECK(allocator.get_largest_free_run() == 20);
	CHECK(allocator.get_allocation_count() == 5);
	if (allocator.get_free_run_count() != before.size()) { FAIL("whole-group dry run changed the free-run list"); return; }
	for (uint32_t index = 0; index < before.size(); ++index) {
		GaussianAtlasAllocator::PageRun after;
		if (!allocator.get_free_run(index, after)) { FAIL("whole-group dry run lost a free run"); return; }
		CHECK(after.first_page == before[index].first_page);
		CHECK(after.page_count == before[index].page_count);
	}
	const uint64_t live_keys[5] = { 1, 3, 5, 6, 8 };
	const uint32_t live_first[5] = { 0, 21, 32, 38, 45 };
	const uint32_t live_pages[5] = { 1, 1, 6, 1, 1 };
	for (uint32_t index = 0; index < 5; ++index) {
		GaussianAtlasAllocator::PageRun run;
		if (!allocator.get_run(live_keys[index], run)) { FAIL("whole-group dry run lost an allocated run"); return; }
		CHECK(run.first_page == live_first[index]);
		CHECK(run.page_count == live_pages[index]);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] atlas dry run matches real allocation failure in the same member order (#1483 C)") {
	auto prepare = [](GaussianAtlasAllocator &r_allocator) {
		r_allocator.reset(11);
		uint32_t first = 0;
		if (!r_allocator.allocate_slot(1, 6, first) || !r_allocator.allocate_slot(2, 1, first) ||
				!r_allocator.allocate_slot(3, 4, first)) { return false; }
		r_allocator.release_slot(1);
		r_allocator.release_slot(3);
		return true;
	};
	GaussianAtlasAllocator allocator, scratch_allocator;
	if (!prepare(allocator) || !prepare(scratch_allocator)) { FAIL("could not prepare two independent six/four-hole allocators"); return; }
	const uint32_t requests[3] = { 3, 3, 4 };
	uint32_t simulated_failure = 0;
	uint64_t simulated_placed = 0;
	const bool simulated_fit = allocator.can_allocate_all(requests, 3, &simulated_failure, &simulated_placed);
	bool actual_fit = true;
	uint32_t actual_failure = 0;
	uint64_t actual_placed = 0;
	for (uint32_t index = 0; index < 3; ++index) {
		uint32_t first = 0;
		if (!scratch_allocator.allocate_slot(10 + index, requests[index], first)) {
			actual_fit = false;
			actual_failure = requests[index];
			break;
		}
		actual_placed += requests[index];
	}
	CHECK_FALSE(actual_fit);
	CHECK(actual_failure == 4);
	CHECK(actual_placed == 6);
	CHECK(simulated_fit == actual_fit);
	CHECK(simulated_failure == actual_failure);
	CHECK(simulated_placed == actual_placed);
	CHECK(requests[0] == 3);
	CHECK(requests[1] == 3);
	CHECK(requests[2] == 4);
	CHECK(allocator.get_free_page_count() == 10);
	CHECK(allocator.get_used_page_count() == 1);
	CHECK(allocator.get_allocation_count() == 1);
	if (allocator.get_free_run_count() != 2) { FAIL("dry run changed the six/four-hole free-run count"); return; }
	GaussianAtlasAllocator::PageRun first_hole, second_hole, barrier;
	if (!allocator.get_free_run(0, first_hole) || !allocator.get_free_run(1, second_hole) || !allocator.get_run(2, barrier)) {
		FAIL("dry run changed the six/four-hole allocator's runs"); return;
	}
	CHECK(first_hole.first_page == 0);
	CHECK(first_hole.page_count == 6);
	CHECK(second_hole.first_page == 7);
	CHECK(second_hole.page_count == 4);
	CHECK(barrier.first_page == 6);
	CHECK(barrier.page_count == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] compaction repairs whole-group fragmentation when every requested node fits individually (#1483 C)") {
	for (bool legacy : { false, true }) {
		CAPTURE(legacy);
		using Streamer = HlodNodeStreamer;
		constexpr uint32_t source = 4, first_child = 8, second_child = 9;
		constexpr uint64_t first_blocked_frame = 100, first_copy_frame = first_blocked_frame + 7;
		Ref<GaussianSplatHlodRuntimeData> data;
		data.instantiate();
		if (data->nodes.resize(10) != OK) { FAIL("could not size group compaction fixture"); return; }
		const uint32_t sizes[10] = { 1, 20, 1, 10, 6, 1, 6, 1, 16, 16 };
		for (uint32_t node = 0; node < 10; ++node) {
			auto &topology = data->nodes.write[node].topology;
			topology.payload_count = sizes[node] * 1024;
			topology.parent = node == 0 ? gs_hlod::kNoParent : (node >= first_child ? source : 0);
			topology.height = node == 0 ? 2 : (node == source ? 1 : 0);
		}
		Streamer streamer;
		Streamer::Config cfg;
		cfg.atlas_pages = 47;
		cfg.retire_delay_frames = 3;
		if (!streamer.init(data, cfg, 1, 1)) { FAIL("group compaction streamer initialization failed"); return; }
		Streamer::TestControl control;
		control.cpu_upload_sink = true;
		if (streamer._test_set_control(control) != OK) { FAIL("group compaction requires CPU device-copy observations"); return; }
		Residency &res = streamer._test_residency();
		for (uint32_t node = 0; node < 8; ++node) {
			if (!make_resident_at(res, node, node + 1) || streamer._test_seed_resident_metadata(node) != OK) {
				FAIL("could not populate group compaction runs"); return;
			}
		}
		LocalVector<Residency::Ticket> tickets;
		res.begin_frame(20);
		res.request(nullptr, 0, tickets);
		if (!res.evict(1) || !res.evict(3) || !res.evict(6)) { FAIL("could not remove group fixture fillers"); return; }
		res.begin_frame(23);
		// Free holes are 20@1, 10@22, 6@39 and scratch 1@46. Two 16-page siblings each
		// fit the 20-page hole individually; sequential placement of their whole group cannot fit.
		CHECK(res.stats().pages_free == 37);
		CHECK(res.stats().largest_free_run == 20);
		CHECK(res.stats().largest_free_run >= res.pages_for(first_child));
		CHECK(res.stats().largest_free_run >= res.pages_for(second_child));
		CHECK(res.stats().pages_free >= res.pages_for(first_child) + res.pages_for(second_child));
		const uint32_t drawn[4] = { 0, 2, 5, 7 };
		const uint32_t demand[2] = { first_child, second_child };
		Streamer::Buffers buffers;
		auto step = [&](uint64_t p_token) {
			streamer.begin_server_frame(p_token, nullptr, &buffers, drawn, 4);
			streamer.begin_frame(nullptr, &buffers, p_token);
			res.protect_drawn(drawn, 4);
			tickets.clear();
			const auto result = legacy ? res.request_legacy(demand, 2, tickets) : res.request(demand, 2, tickets);
			streamer._test_compaction_step(nullptr, buffers, drawn, 4);
			return result;
		};
		uint64_t refused = 0;
		for (uint64_t token = first_blocked_frame; token <= first_copy_frame; ++token) {
			const auto result = step(token);
			CHECK(result.requested == 0);
			CHECK(tickets.is_empty());
			if (token == first_blocked_frame) { refused = res.stats().allocation_refused_total; }
			if (!legacy && token < first_copy_frame) {
				CHECK(res.stats().allocation_refused_total == refused); // The unchanged B group uses cached coarse_blocked.
			}
			if (token < first_copy_frame) {
				CHECK_FALSE(res.compaction_active());
				CHECK(res.stats().compaction_pages_moved_total == 0);
				CHECK(res.stats().compaction_frames_total == 0);
				CHECK(res.stats().compaction_moves_total == 0);
				CHECK(res.stats().compaction_no_candidate_frames_total == 0);
			}
		}
		CHECK(refused > 0);
		CHECK(res.stats().compaction_pages_moved_total == 4);
		CHECK(res.stats().compaction_frames_total == 1); // The eighth blocked server frame starts the copy.
		CHECK_FALSE(res.compaction_active()); // Actual free pages are below 32 while both runs are charged.
		CHECK(res.compaction_move_needed());
		CHECK(res.compaction_move().node == source);
		CHECK(res.compaction_move().source_first == 32);
		CHECK(res.compaction_move().destination_first == 39);
		CHECK(res.compaction_move().copied_pages == 4);
		step(first_copy_frame + 1);
		CHECK(res.stats().compaction_pages_moved_total == 6);
		CHECK(res.stats().pages_free == 30); // The scratch page and duplicate run temporarily consume admission headroom.
		CHECK(res.stats().pages_in_flight == 7);
		CHECK(res.allocator_used_pages() == 17);
		for (uint64_t token = first_copy_frame + 2; token < first_copy_frame + 4; ++token) {
			step(token);
			CHECK(res.compaction_move().valid()); // A budget shortfall caused by its own destination must not cancel the copy.
			CHECK(res.stats().compaction_moves_total == 0);
			CHECK(res.allocator_used_pages() == 17);
		}
		for (uint64_t token = first_copy_frame + 4; token < first_copy_frame + 7; ++token) {
			step(token);
			CHECK(res.stats().compaction_moves_total == 1);
			CHECK(res.stats().pages_retiring == 6);
			CHECK(tickets.is_empty());
		}
		step(first_copy_frame + 7); // Retiring the old source leaves distinct 20-page and 16-page holes.
		CHECK(res.stats().compaction_moves_total == 1);
		CHECK(res.stats().compaction_pages_moved_total == 6);
		if (tickets.size() != 2) { FAIL("compaction must admit both sixteen-page siblings as one whole group"); return; }
		CHECK(tickets[0].node == first_child);
		CHECK(tickets[1].node == second_child);
		for (const auto &ticket : tickets) {
			CHECK(res.begin_upload(ticket));
			CHECK(res.finish_upload(ticket, true));
		}
		CHECK(res.state(first_child) == Residency::RESIDENT);
		CHECK(res.state(second_child) == Residency::RESIDENT);
		const auto stats = res.stats();
		CHECK(stats.pages_resident + stats.pages_in_flight + stats.pages_retiring == res.allocator_used_pages());
		CHECK(res.allocator_used_pages() + stats.pages_free == 47);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] a later capped legacy subset preserves the full blocked group's compaction streak (#1483 C)") {
	Vector<Residency::NodeInfo> infos;
	if (infos.resize(12) != OK) { FAIL("could not size multi-view blocked-group fixture"); return; }
	const uint32_t sizes[12] = { 1, 20, 1, 10, 6, 1, 6, 1, 16, 16, 1, 1 };
	for (uint32_t node = 0; node < 12; ++node) {
		auto &info = infos.write[node];
		info.splat_count = sizes[node] * 1024;
		info.parent = node == 0 ? UINT32_MAX : (node == 8 || node == 9 ? 4 : (node == 10 || node == 11 ? 2 : 0));
		info.height = node == 0 ? 2 : (node == 2 || node == 4 ? 1 : 0);
	}
	auto cfg = config(47, 0);
	cfg.max_requests_per_frame = 2;
	Residency res;
	if (!res.reset(infos, cfg, 1, 1)) { FAIL("could not initialize multi-view blocked-group fixture"); return; }
	for (uint32_t node = 0; node < 8; ++node) {
		if (!make_resident_at(res, node, node + 1)) { FAIL("could not populate multi-view blocked-group runs"); return; }
	}
	LocalVector<Residency::Ticket> tickets;
	res.begin_frame(20);
	res.request(nullptr, 0, tickets);
	if (!res.evict(1) || !res.evict(3) || !res.evict(6)) { FAIL("could not fragment multi-view blocked-group fixture"); return; }
	const uint32_t drawn[4] = { 0, 2, 5, 7 };
	const uint32_t full_group[2] = { 8, 9 };
	const uint32_t dummy_group[2] = { 10, 11 };
	const uint32_t fitting_subset = 8;
	for (uint64_t token = 100; token <= 107; ++token) {
		res.begin_frame(token);
		res.protect_drawn(drawn, 4);
		tickets.clear();
		const auto blocked = res.request_legacy(full_group, 2, tickets);
		CHECK(blocked.requested == 0);
		CHECK(blocked.fragmented_groups == 1);
		CHECK(tickets.is_empty());
		if (token < 107) { CHECK_FALSE(res.compaction_active()); }
		else { CHECK(res.compaction_active()); }
		// Consume the existing two-request allowance with another parent's real B tickets, then
		// return their pages immediately. This leaves the original holes and request cap intact.
		tickets.clear();
		const auto dummies = res.request(dummy_group, 2, tickets);
		CHECK(dummies.requested == 2);
		if (tickets.size() != 2) { FAIL("dummy admissions must consume exactly two request permits"); return; }
		for (const auto &ticket : tickets) {
			if (!res.begin_upload(ticket) || !res.finish_upload(ticket, true)) {
				FAIL("could not complete the dummy request permits"); return;
			}
		}
		res.release_transition_leases();
		if (!res.evict(10) || !res.evict(11)) { FAIL("could not return undrawn dummy runs"); return; }
		CHECK(res.stats().pages_free == 37);
		CHECK(res.stats().largest_free_run == 20);
		// Another view wants one fitting member of the same parent. Its capped subset cannot
		// replace the earlier failed set {8,9}, or advance that set's streak twice in one token.
		tickets.clear();
		const auto subset = res.request_legacy(&fitting_subset, 1, tickets);
		CHECK(subset.capped);
		CHECK(subset.requested == 0);
		CHECK(tickets.is_empty());
		if (token < 107) { CHECK_FALSE(res.compaction_active()); }
		else { CHECK(res.compaction_active()); }
	}
	if (!res.acquire_compaction_scratch()) { FAIL("eighth blocked server frame must reserve compaction scratch"); return; }
	if (!res.plan_compaction(drawn, 4)) { FAIL("the retained full blocked set must permit useful undrawn compaction"); return; }
	CHECK(res.compaction_move().node == 4);
	CHECK(res.compaction_move().source_first == 32);
	CHECK(res.compaction_move().destination_first == 39);
	CHECK(res.compaction_move().pages == 6);
	CHECK(res.allocator_used_pages() == 17);
	const auto stats = res.stats();
	CHECK(stats.pages_resident + stats.pages_in_flight + stats.pages_retiring == res.allocator_used_pages());
}

// The same physical holes distinguish a budget refusal (5+5 > 9 free) from a fragmented
// group (4+4 <= 9 free) whose only undrawn source would merely exchange equally useful holes.
struct CompactionRefusalFixture {
	using Streamer = HlodNodeStreamer;
	Ref<GaussianSplatHlodRuntimeData> data;
	Streamer streamer;
	Streamer::Buffers buffers;
	LocalVector<Residency::Ticket> tickets;
	uint32_t drawn[4] = { 0, 2, 5, 7 };

	Residency &residency() { return streamer._test_residency(); }

	bool init(uint32_t p_requested_pages) {
		data.instantiate();
		if (data->nodes.resize(10) != OK) { FAIL("could not size compaction refusal fixture"); return false; }
		const uint32_t pages[10] = { 1, 5, 1, 2, 2, 1, 1, 1, p_requested_pages, p_requested_pages };
		for (uint32_t node = 0; node < 10; ++node) {
			auto &topology = data->nodes.write[node].topology;
			topology.payload_count = pages[node] * 1024;
			topology.parent = node == 0 ? gs_hlod::kNoParent : (node >= 8 ? 4 : 0);
			topology.height = node == 0 ? 2 : (node == 4 ? 1 : 0);
		}
		Streamer::Config cfg;
		cfg.atlas_pages = 15;
		cfg.retire_delay_frames = 0;
		if (!streamer.init(data, cfg, 1, 1)) { FAIL("could not initialize compaction refusal fixture"); return false; }
		Streamer::TestControl control;
		control.cpu_upload_sink = true;
		if (streamer._test_set_control(control) != OK) { FAIL("compaction refusal fixture requires the CPU device sink"); return false; }
		for (uint32_t node = 0; node < 8; ++node) {
			if (!make_resident_at(residency(), node, node + 1) || streamer._test_seed_resident_metadata(node) != OK) {
				FAIL("could not populate compaction refusal runs and metadata"); return false;
			}
		}
		residency().begin_frame(20);
		residency().request(nullptr, 0, tickets);
		if (!residency().evict(1) || !residency().evict(3) || !residency().evict(6)) {
			FAIL("could not remove compaction refusal fillers"); return false;
		}
		// Root@0, free5@1, drawn@6, free2@7, source2@9, drawn@11, free1@12,
		// drawn@13, free1@14. The group's parent is the undrawn source, so normal
		// admission cannot evict it; every other resident is drawn for each test frame.
		if (residency().stats().pages_free != 9 || residency().stats().largest_free_run != 5) {
			FAIL("compaction refusal fixture requires free runs of 5,2,1,1 pages"); return false;
		}
		return true;
	}

	Residency::RequestResult step(uint64_t p_token, bool p_legacy = false) {
		streamer.begin_server_frame(p_token, nullptr, &buffers, drawn, 4);
		streamer.begin_frame(nullptr, &buffers, p_token);
		residency().protect_drawn(drawn, 4);
		tickets.clear();
		const uint32_t demand[2] = { 8, 9 };
		const auto result = p_legacy ? residency().request_legacy(demand, 2, tickets) : residency().request(demand, 2, tickets);
		streamer._test_compaction_step(nullptr, buffers, drawn, 4);
		return result;
	}

	bool runs_unchanged() {
		const uint32_t nodes[5] = { 0, 2, 4, 5, 7 };
		const uint32_t expected_first[5] = { 0, 6, 9, 11, 13 };
		const uint32_t expected_pages[5] = { 1, 1, 2, 1, 1 };
		for (uint32_t index = 0; index < 5; ++index) {
			uint32_t first = 0, pages = 0;
			ChunkMetaGPU meta = {};
			if (!residency().run(nodes[index], first, pages) || first != expected_first[index] || pages != expected_pages[index] ||
					!streamer._test_compaction_meta(nodes[index], meta) || meta.atlas_base != expected_first[index] * 1024) { return false; }
		}
		return true;
	}
};

TEST_CASE("[GaussianSplatting][HLOD][Residency] protected-capacity budget refusals never activate group compaction (#1483 C)") {
	for (bool legacy : { false, true }) {
		CAPTURE(legacy);
		CompactionRefusalFixture h;
		if (!h.init(5)) { return; }
		for (uint64_t token = 100; token < 116; ++token) { // Sixteen synchronous server frames exceed the R=8 trigger.
			const auto result = h.step(token, legacy);
			const auto stats = h.residency().stats();
			CHECK(result.requested == 0);
			CHECK(result.fragmented_groups == 0);
			CHECK(h.tickets.is_empty());
			CHECK(stats.pages_free < h.residency().pages_for(8) + h.residency().pages_for(9));
			CHECK(stats.allocation_refused_total > 0);
			CHECK(stats.allocation_protected_capacity_total > 0);
			CHECK(stats.allocation_over_page_budget_total == 0);
			CHECK(stats.allocation_no_contiguous_run_total == 0);
			CHECK_FALSE(h.residency().compaction_active());
			CHECK_FALSE(h.residency().has_compaction_scratch());
			CHECK_FALSE(h.residency().compaction_move().valid());
			CHECK(stats.compaction_pages_moved_total == 0);
			CHECK(stats.compaction_moves_total == 0);
			CHECK(stats.compaction_frames_total == 0);
			CHECK(stats.compaction_no_candidate_frames_total == 0);
			CHECK(stats.evicted_total == 3); // Only the deliberate fixture fillers were evicted.
			CHECK(stats.pages_free == 9);
			CHECK(h.residency().allocator_used_pages() == 6);
			CHECK(h.runs_unchanged());
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Residency] active group compaction refuses a move that cannot improve the placed prefix (#1483 C)") {
	CompactionRefusalFixture h;
	if (!h.init(4)) { return; }
	for (uint64_t token = 100; token <= 111; ++token) {
		const auto result = h.step(token);
		const auto stats = h.residency().stats();
		CHECK(result.requested == 0);
		CHECK(h.tickets.is_empty());
		if (token < 107) {
			CHECK_FALSE(h.residency().compaction_active());
			CHECK(stats.compaction_no_candidate_frames_total == 0);
		} else {
			CHECK(h.residency().compaction_active()); // Threshold-disabled mutation must fail by assertion.
			CHECK(stats.compaction_no_candidate_frames_total == token - 106);
			CHECK(h.residency().has_compaction_scratch());
			CHECK(h.residency().allocator_used_pages() == 7);
			CHECK(stats.pages_free == 8);
		}
		// Moving the two-page source@9 into free5@1 shrinks that hole to 3 and grows
		// free2+source2 to 4. This swaps which hole fits one four-page member: the placed
		// prefix stays 4, and the second member still fails. There is no strict progress.
		CHECK_FALSE(h.residency().compaction_move().valid());
		CHECK(stats.compaction_pages_moved_total == 0);
		CHECK(stats.compaction_moves_total == 0);
		CHECK(stats.compaction_frames_total == 0);
		CHECK(stats.compaction_cap_limited_frames_total == 0);
		CHECK(stats.evicted_total == 3);
		CHECK(h.runs_unchanged());
		CHECK(stats.pages_resident + stats.pages_in_flight + stats.pages_retiring == h.residency().allocator_used_pages());
	}
}

}
