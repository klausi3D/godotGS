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
		CHECK_FALSE(sim.res.fail_load(t)); // And a late read failure of the same request.
		CHECK(sim.res.stats().late_dropped_cancelled_total == 2);
		CHECK(sim.res.stats().late_dropped_total == 0);
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

}
