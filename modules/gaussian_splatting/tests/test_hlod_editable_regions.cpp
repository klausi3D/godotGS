extern "C" int test_hlod_editable_regions_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/gaussian_splat_world_paged.h"
#include "../lod/hlod_edit.h"
#include "../lod/hlod_node_residency.h"
#include "../lod/hlod_runtime_selection.h"
#include "../renderer/hlod_node_pack.h"
#include "../renderer/hlod_node_streamer.h"
#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "scene/main/scene_tree.h"
#include "servers/rendering/rendering_device.h"
#include <algorithm>
#include <functional>
#include <vector>

// ADR 14.5, package F1: editable regions in a baked or paged HLOD world. CPU only. The synthetic trees drive the real
// selector and the real residency model; the baked-world tests drive the real edit factory, the V4 writer/reader and
// the real node streamer jobs. The publication boundary on a device is covered by the RequiresGPU case.
namespace TestHlodEditableRegions {
// Neither REQUIRE nor CHECK aborts in this build (DOCTEST_CONFIG_NO_EXCEPTIONS): a failed requirement must leave the test
// before the next line dereferences what it was guarding.
#define GS_NEED(cond) 	do { 		if (!(cond)) { 			FAIL("requirement failed: ", #cond); 			return; 		} 	} while (0)

using namespace gs_hlod_runtime;
using Residency = NodeResidency;

// ---------------------------------------------------------------------------------------------------------------------
// Synthetic complete binary tree (BFS order): node i has children 2i+1, 2i+2. Interiors hold 1000 splats (1 page), leaves
// 1500 (2 pages). Nodes sit on a line so the camera distance decides what the quality rule wants.
// ---------------------------------------------------------------------------------------------------------------------
static void binary_tree(GaussianSplatHlodRuntimeData &r_data, uint32_t p_levels) {
	const uint32_t count = (1u << p_levels) - 1u;
	const uint32_t first_leaf = (1u << (p_levels - 1)) - 1u;
	r_data.nodes.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		auto &n = r_data.nodes.write[i];
		uint32_t level = 0;
		while (((1u << (level + 1)) - 1u) <= i) { ++level; }
		const uint32_t height = p_levels - 1 - level;
		const uint32_t span = 1u << height;
		const uint32_t first = (i - ((1u << level) - 1u)) * span;
		n.bounds_center = Vector3((float(first) + float(span) * 0.5f) * 10.0f, 0, -10);
		n.topology.radius = float(span) * 5.0f;
		n.topology.height = height;
		n.topology.payload_count = i >= first_leaf ? 1500 : 1000;
		n.topology.geometric_error = height == 0 ? 0.0f : float(1u << height);
		n.topology.parent = i == 0 ? gs_hlod::kNoParent : (i - 1) / 2;
		if (i < first_leaf) {
			n.topology.first_child = 2 * i + 1;
			n.topology.child_count = 2;
		}
	}
}

// Marks the ancestors of `p_leaves` stale and the leaves edited, exactly as lod/hlod_edit.cpp does for a real edit.
static void mark_edited(GaussianSplatHlodRuntimeData &r_data, std::initializer_list<uint32_t> p_leaves, uint64_t p_revision = 1) {
	r_data.edit_revision = p_revision;
	r_data.edit_state.resize(r_data.nodes.size());
	r_data.edit_state.fill(GaussianSplatHlodRuntimeData::EDIT_CLEAN);
	for (uint32_t leaf : p_leaves) {
		r_data.edit_state.write[leaf] = GaussianSplatHlodRuntimeData::EDIT_LEAF;
		GaussianSplatHlodRuntimeData::EditedLeaf e;
		e.node = leaf;
		e.revision = p_revision;
		r_data.edited.push_back(e);
		for (uint32_t up = r_data.nodes[leaf].topology.parent; up != gs_hlod::kNoParent; up = r_data.nodes[up].topology.parent) {
			r_data.edit_state.write[up] = GaussianSplatHlodRuntimeData::EDIT_STALE;
		}
	}
}

static CutRequest base_request(uint32_t p_nodes) {
	CutRequest r;
	r.frustum_enabled = false;
	r.focal_pixels = 100;
	r.tau_pixels = 1;
	r.record_budget = 1.0e9;
	r.splat_budget = 1u << 30;
	r.node_budget = p_nodes;
	return r;
}

static std::vector<uint32_t> cut_of(const CutHistory &p_history, const CutHistory::Result &p_result) {
	std::vector<uint32_t> nodes;
	for (uint32_t i = 0; i < p_result.selected_count; ++i) { nodes.push_back(p_history.selected_node(i)); }
	std::sort(nodes.begin(), nodes.end());
	return nodes;
}

// Every leaf has exactly one selected ancestor-or-self.
static bool exact_cover(const GaussianSplatHlodRuntimeData &p_data, const std::vector<uint32_t> &p_cut) {
	for (uint32_t leaf = 0; leaf < uint32_t(p_data.nodes.size()); ++leaf) {
		if (p_data.nodes[leaf].topology.child_count != 0) { continue; }
		uint32_t hits = 0;
		for (uint32_t a = leaf; a != gs_hlod::kNoParent; a = p_data.nodes[a].topology.parent) {
			hits += std::count(p_cut.begin(), p_cut.end(), a) ? 1u : 0u;
		}
		if (hits != 1) { return false; }
	}
	return true;
}

TEST_CASE("[GaussianSplatting][HLOD][Edit] the selector never draws a stale ancestor and expands the path to the edited leaf") {
	GaussianSplatHlodRuntimeData control;
	binary_tree(control, 4);
	GaussianSplatHlodRuntimeData edited;
	binary_tree(edited, 4);
	mark_edited(edited, { 9 }); // Path 9 -> 4 -> 1 -> 0; replacement children are 10, 3 and 2.
	CutHistory history;
	GS_NEED(history.reserve(15));
	CutRequest request = base_request(15);
	request.tau_pixels = 1.0e9; // Quality alone wants the root.

	// Control: without an edit the coarse root is the cut.
	auto base = history.select(control, request);
	CHECK(base.status == CutHistory::COMPLETE);
	CHECK(cut_of(history, base) == std::vector<uint32_t>{ 0 });
	CHECK_FALSE(base.edit_forced);

	// Edited, with every non-stale node resident and the stale ones deliberately NOT: a stale node needs no residency.
	uint8_t resident[15];
	for (int i = 0; i < 15; ++i) { resident[i] = 1; }
	resident[0] = resident[1] = resident[4] = 0;
	request.resident = resident;
	request.resident_count = 15;
	history.invalidate();
	auto cut = history.select(edited, request);
	GS_NEED(cut.status == CutHistory::COMPLETE);
	CHECK(cut.edit_forced);
	CHECK(cut_of(history, cut) == (std::vector<uint32_t>{ 2, 3, 9, 10 }));
	CHECK(exact_cover(edited, cut_of(history, cut)));
	CHECK(cut.selected_splats == 1000 + 1000 + 1500 + 1500 - 0); // 2, 3 interiors; 9, 10 leaves.
	CHECK(history.demand_count() == 0);

	// A fine view (the quality rule wants the leaves): the cut is the leaf level, still no stale node.
	request.tau_pixels = 0.001;
	history.invalidate();
	auto fine = history.select(edited, request);
	GS_NEED(fine.status == CutHistory::COMPLETE);
	const std::vector<uint32_t> fine_cut = cut_of(history, fine);
	CHECK(exact_cover(edited, fine_cut));
	for (uint32_t node : fine_cut) { CHECK_FALSE(edited.is_stale(node)); }
}

TEST_CASE("[GaussianSplatting][HLOD][Edit] a missing replacement child or a cap that cannot hold the replacement yields no cut, never the stale parent") {
	GaussianSplatHlodRuntimeData edited;
	binary_tree(edited, 4);
	mark_edited(edited, { 9 });
	CutHistory history;
	GS_NEED(history.reserve(15));
	CutRequest request = base_request(15);
	request.tau_pixels = 1.0e9;
	uint8_t resident[15];
	for (int i = 0; i < 15; ++i) { resident[i] = 1; }
	resident[0] = resident[1] = resident[4] = 0;
	resident[3] = 0; // The replacement child of stale node 1 is not there.
	request.resident = resident;
	request.resident_count = 15;
	auto waiting = history.select(edited, request);
	CHECK(waiting.status == CutHistory::BOOTSTRAP_MISSING_RESIDENCY);
	CHECK(waiting.selected_count == 0);
	bool wants_missing = false, wants_stale = false;
	for (uint32_t i = 0; i < history.demand_count(); ++i) {
		wants_missing = wants_missing || history.demand_node(i) == 3;
		wants_stale = wants_stale || edited.is_stale(history.demand_node(i));
	}
	CHECK(wants_missing);
	CHECK_FALSE(wants_stale); // A stale node is never demanded.

	resident[3] = 1;
	request.node_budget = 3; // The replacement cut {2, 3, 9, 10} needs four nodes.
	history.invalidate();
	auto capped = history.select(edited, request);
	CHECK(capped.status == CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(capped.selected_count == 0);
	CHECK(capped.selected_splats == 0);

	request.node_budget = 15;
	request.splat_budget = 3000; // Drawn splats of the replacement cut are 5000.
	history.invalidate();
	auto small = history.select(edited, request);
	CHECK(small.status != CutHistory::COMPLETE);
	CHECK(small.selected_count == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit] an edit of the root leaf and a tree edited at several leaves stay complete") {
	// Several edited leaves in different branches: both paths are expanded, the cut covers every leaf once.
	GaussianSplatHlodRuntimeData edited;
	binary_tree(edited, 4);
	mark_edited(edited, { 7, 14 });
	CutHistory history;
	GS_NEED(history.reserve(15));
	CutRequest request = base_request(15);
	request.tau_pixels = 1.0e9;
	auto cut = history.select(edited, request);
	GS_NEED(cut.status == CutHistory::COMPLETE);
	const std::vector<uint32_t> nodes = cut_of(history, cut);
	CHECK(exact_cover(edited, nodes));
	for (uint32_t node : nodes) { CHECK_FALSE(edited.is_stale(node)); }
	CHECK(std::count(nodes.begin(), nodes.end(), 7u) == 1);
	CHECK(std::count(nodes.begin(), nodes.end(), 14u) == 1);
	// A single-node tree whose only node is the edited leaf has no ancestor: it is drawn as is.
	GaussianSplatHlodRuntimeData single;
	single.nodes.resize(1);
	single.nodes.write[0].topology.payload_count = 100;
	single.nodes.write[0].bounds_center = Vector3(0, 0, -10);
	mark_edited(single, { 0 });
	CutHistory one;
	GS_NEED(one.reserve(1));
	auto result = one.select(single, base_request(1));
	CHECK(result.status == CutHistory::COMPLETE);
	CHECK(result.selected_count == 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// Residency: staged edits.
// ---------------------------------------------------------------------------------------------------------------------
static Residency::Config residency_config(uint32_t p_pages) {
	Residency::Config c;
	c.page_capacity = p_pages;
	c.retire_delay_frames = 2;
	c.max_requests_per_frame = 64;
	c.evictions_per_frame = 64;
	return c;
}

// A residency table with the whole 4-level tree resident (7 interiors x 1 page + 8 leaves x 2 pages = 23 pages).
struct EditSim {
	Ref<GaussianSplatHlodRuntimeData> data;
	Residency res;
	uint64_t frame = 0;
	bool init(uint32_t p_pages) {
		data.instantiate();
		binary_tree(*data.ptr(), 4);
		Vector<Residency::NodeInfo> infos;
		if (!make_node_infos(*data.ptr(), infos) || !res.reset(infos, residency_config(p_pages), 1, 1)) { return false; }
		res.begin_frame(++frame);
		std::vector<uint32_t> all;
		for (uint32_t i = 0; i < 15; ++i) { all.push_back(i); }
		LocalVector<Residency::Ticket> tickets;
		// One group per parent, the way the selector demands them; the root is its own group.
		for (uint32_t parent = 0; parent < 15; parent += 1) {
			std::vector<uint32_t> group;
			if (parent == 0) { group.push_back(0); }
			if (parent < 7) { group.push_back(2 * parent + 1); group.push_back(2 * parent + 2); }
			res.request(group.data(), uint32_t(group.size()), tickets);
		}
		for (uint32_t i = 0; i < tickets.size(); ++i) {
			if (res.begin_upload(tickets[i])) { res.finish_upload(tickets[i], true); }
		}
		return res.stats().resident == 15;
	}
	void step() { res.begin_frame(++frame); }
};

TEST_CASE("[GaussianSplatting][HLOD][Edit][Residency] a staged edit changes nothing drawable until every shadow and replacement child is ready, then flips atomically") {
	EditSim sim;
	GS_NEED(sim.init(40));
	const Residency::Stats before = sim.res.stats();
	GS_NEED(before.resident == 15 && before.pages_resident == 23);
	uint32_t old_first = 0, old_pages = 0;
	GS_NEED(sim.res.run(9, old_first, old_pages));
	GS_NEED(old_pages == 2);

	LocalVector<Residency::Ticket> tickets;
	const Residency::EditedLeaf leaf = { 9, 3000 }; // Three pages: the replacement is bigger than the old leaf.
	GS_NEED(sim.res.stage_edit(1, &leaf, 1, tickets) == Residency::EDIT_OK);
	GS_NEED(tickets.size() == 1);
	CHECK(tickets[0].shadow);
	CHECK(tickets[0].page_count == 3);
	CHECK(sim.res.is_edit_staged());
	// Staged only: the old cut is intact (mask, states, run), the shadow pages are reserved and counted.
	CHECK(sim.res.resident_mask()[0] == 1);
	CHECK(sim.res.resident_mask()[9] == 1);
	CHECK(sim.res.state(9) == Residency::RESIDENT);
	uint32_t first = 0, pages = 0;
	CHECK(sim.res.run(9, first, pages));
	CHECK(first == old_first);
	CHECK(pages == old_pages);
	CHECK(sim.res.stats().pages_shadow == 3);
	CHECK(sim.res.stats().pages_free == before.pages_free - 3);
	CHECK_FALSE(sim.res.edit_ready()); // The shadow payload has not arrived.
	CHECK_FALSE(sim.res.commit_edit()); // And a commit that is not ready changes nothing.
	CHECK(sim.res.resident_mask()[0] == 1);
	// The replacement children (2, 3, 10) are held against eviction; an unrelated node is not.
	CHECK(sim.res.is_protected(2));
	CHECK(sim.res.is_protected(3));
	CHECK(sim.res.is_protected(10));
	CHECK_FALSE(sim.res.evict(10));
	// A foreign or malformed ticket cannot complete the shadow.
	Residency::Ticket wrong = tickets[0];
	wrong.request_generation += 1;
	CHECK_FALSE(sim.res.finish_shadow(wrong, true));
	Residency::Ticket not_shadow = tickets[0];
	not_shadow.shadow = false;
	CHECK_FALSE(sim.res.begin_upload(not_shadow));
	CHECK_FALSE(sim.res.edit_ready());
	CHECK(sim.res.is_shadow_pending(tickets[0]));
	CHECK(sim.res.finish_shadow(tickets[0], true));
	CHECK_FALSE(sim.res.is_shadow_pending(tickets[0]));
	CHECK(sim.res.edit_ready());
	sim.step();

	const uint64_t evicted_before = sim.res.stats().evicted_total;
	GS_NEED(sim.res.commit_edit());
	CHECK_FALSE(sim.res.is_edit_staged());
	CHECK(sim.res.committed_edit_revision() == 1);
	// The flip: stale ancestors are gone from the mask and the model, the edited leaf is resident on its NEW run.
	CHECK(sim.res.resident_mask()[0] == 0);
	CHECK(sim.res.resident_mask()[1] == 0);
	CHECK(sim.res.resident_mask()[4] == 0);
	CHECK(sim.res.resident_mask()[9] == 1);
	CHECK(sim.res.resident_mask()[2] == 1);
	CHECK(sim.res.state(9) == Residency::RESIDENT);
	CHECK(sim.res.state(0) == Residency::ABSENT);
	CHECK(sim.res.is_stale(0));
	CHECK_FALSE(sim.res.is_protected(0)); // The root is no longer pinned: it is no valid fallback.
	GS_NEED(sim.res.run(9, first, pages));
	CHECK(first == tickets[0].first_page);
	CHECK(pages == 3);
	CHECK(sim.res.pages_for(9) == 3);
	CHECK(sim.res.stats().pages_shadow == 0);
	CHECK(sim.res.stats().stale_nodes == 3);
	CHECK(sim.res.stats().evicted_total == evicted_before + 3);
	// The old leaf run and the stale runs wait out the retirement delay before reuse (old frames may still read them).
	CHECK(sim.res.stats().pages_retiring == 2 + 3);
	LocalVector<uint32_t> evicted;
	sim.res.take_evicted(evicted);
	std::vector<uint32_t> cleared(evicted.ptr(), evicted.ptr() + evicted.size());
	std::sort(cleared.begin(), cleared.end());
	CHECK(cleared == (std::vector<uint32_t>{ 0, 1, 4 })); // Metadata cleared for the stale nodes; the edited leaf keeps its new one.
	// A stale node is never requested again.
	LocalVector<Residency::Ticket> again;
	const uint32_t demand_root[1] = { 0 };
	const Residency::RequestResult requested = sim.res.request(demand_root, 1, again);
	CHECK(requested.requested == 0);
	CHECK(requested.blocked_groups == 1);
	CHECK(again.size() == 0);
	CHECK(sim.res.state(0) == Residency::ABSENT);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][Residency] an edit invalidates the in-flight load of the old content with the request generation") {
	EditSim sim;
	GS_NEED(sim.init(40));
	// Leaf 9 is evicted and requested again: a load of the OLD content is in flight when the edit is staged.
	sim.step();
	sim.step();
	GS_NEED(sim.res.evict(9));
	LocalVector<Residency::Ticket> old_tickets;
	sim.step();
	const uint32_t demand[2] = { 9, 10 };
	sim.res.request(demand, 2, old_tickets);
	GS_NEED(old_tickets.size() == 1);
	GS_NEED(old_tickets[0].node == 9 && !old_tickets[0].shadow);
	GS_NEED(sim.res.state(9) == Residency::LOADING);

	LocalVector<Residency::Ticket> tickets;
	const Residency::EditedLeaf leaf = { 9, 1000 };
	GS_NEED(sim.res.stage_edit(1, &leaf, 1, tickets) == Residency::EDIT_OK);
	GS_NEED(sim.res.finish_shadow(tickets[0], true));
	GS_NEED(sim.res.commit_edit());
	// The old load completes late: it is dropped, counted, and cannot touch the new version.
	const uint64_t dropped = sim.res.stats().late_dropped_total;
	CHECK_FALSE(sim.res.begin_upload(old_tickets[0]));
	CHECK_FALSE(sim.res.finish_upload(old_tickets[0], true));
	CHECK(sim.res.stats().late_dropped_total == dropped + 2);
	CHECK(sim.res.state(9) == Residency::RESIDENT);
	uint32_t first = 0, pages = 0;
	GS_NEED(sim.res.run(9, first, pages));
	CHECK(first == tickets[0].first_page);
	CHECK(pages == 1);
	CHECK(sim.res.stats().cancelled_total >= 1);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][Residency] an edit is admitted whole or not at all, one at a time, in revision order") {
	EditSim sim;
	GS_NEED(sim.init(26)); // 23 resident pages, 3 free.
	const Residency::Stats before = sim.res.stats();
	GS_NEED(before.pages_free == 3);
	LocalVector<Residency::Ticket> tickets;

	// Malformed edits are INVALID and change nothing.
	const Residency::EditedLeaf interior = { 4, 1000 };
	CHECK(sim.res.stage_edit(1, &interior, 1, tickets) == Residency::EDIT_INVALID); // Not a leaf.
	const Residency::EditedLeaf empty = { 9, 0 };
	CHECK(sim.res.stage_edit(1, &empty, 1, tickets) == Residency::EDIT_INVALID);
	const Residency::EditedLeaf out_of_range = { 99, 1000 };
	CHECK(sim.res.stage_edit(1, &out_of_range, 1, tickets) == Residency::EDIT_INVALID);
	const Residency::EditedLeaf twice[2] = { { 9, 1000 }, { 9, 1000 } };
	CHECK(sim.res.stage_edit(1, twice, 2, tickets) == Residency::EDIT_INVALID);
	const Residency::EditedLeaf ok_leaf = { 9, 1000 };
	CHECK(sim.res.stage_edit(2, &ok_leaf, 1, tickets) == Residency::EDIT_INVALID); // Skips revision 1.
	CHECK(sim.res.stage_edit(0, &ok_leaf, 1, tickets) == Residency::EDIT_INVALID);
	CHECK(tickets.size() == 0);
	CHECK(sim.res.stats().pages_free == before.pages_free);

	// Two leaves that need more pages than are free, with everything protected from eviction: no space, no change.
	sim.step();
	const uint32_t drawn_all[8] = { 7, 8, 9, 10, 11, 12, 13, 14 };
	sim.res.protect_drawn(drawn_all, 8);
	const Residency::EditedLeaf big[2] = { { 9, 3000 }, { 14, 3000 } }; // 6 pages > 3 free.
	CHECK(sim.res.stage_edit(1, big, 2, tickets) == Residency::EDIT_NO_SPACE);
	CHECK(tickets.size() == 0);
	CHECK_FALSE(sim.res.is_edit_staged());
	CHECK(sim.res.stats().pages_free == before.pages_free);
	CHECK(sim.res.stats().pages_shadow == 0);
	CHECK(sim.res.stats().edits_rejected_total == 7);
	sim.step();
	sim.step();
	CHECK_FALSE(sim.res.is_protected(3)); // No hold leaked from the refused edit.

	// A fitting edit is admitted; a second one while it is staged is BUSY; cancelling gives every page back.
	const Residency::EditedLeaf small = { 9, 2500 }; // 3 pages.
	GS_NEED(sim.res.stage_edit(1, &small, 1, tickets) == Residency::EDIT_OK);
	CHECK(sim.res.stage_edit(1, &small, 1, tickets) == Residency::EDIT_BUSY);
	CHECK(sim.res.stats().pages_free == before.pages_free - 3);
	CHECK(sim.res.is_protected(3));
	GS_NEED(sim.res.cancel_edit());
	CHECK_FALSE(sim.res.is_edit_staged());
	CHECK(sim.res.stats().pages_free == before.pages_free);
	CHECK(sim.res.stats().pages_shadow == 0);
	CHECK(sim.res.committed_edit_revision() == 0);
	CHECK(sim.res.resident_mask()[0] == 1); // The world is exactly as before.
	CHECK_FALSE(sim.res.finish_shadow(tickets[0], true)); // The cancelled edit's late payload is dropped.
	CHECK_FALSE(sim.res.cancel_edit());
	sim.step();
	sim.step();
	CHECK_FALSE(sim.res.is_protected(3)); // The hold is gone with the edit.
	// After a cancel the same revision can be staged again.
	CHECK(sim.res.stage_edit(1, &small, 1, tickets) == Residency::EDIT_OK);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][Residency] a replacement child that is not resident yet is prefetched as demand and gates the commit") {
	EditSim sim;
	GS_NEED(sim.init(40));
	sim.step();
	sim.step();
	// Child 3 of stale node 1 is not resident when the edit is staged.
	GS_NEED(sim.res.evict(3));
	sim.step();
	sim.step();
	sim.step();
	LocalVector<Residency::Ticket> tickets;
	const Residency::EditedLeaf leaf = { 9, 1000 };
	GS_NEED(sim.res.stage_edit(1, &leaf, 1, tickets) == Residency::EDIT_OK);
	GS_NEED(sim.res.finish_shadow(tickets[0], true));
	CHECK_FALSE(sim.res.edit_ready()); // Shadow ready, replacement cut incomplete.
	LocalVector<uint32_t> prefetch;
	sim.res.edit_prefetch(prefetch);
	GS_NEED(prefetch.size() == 1);
	CHECK(prefetch[0] == 3);
	LocalVector<Residency::Ticket> loads;
	sim.res.request(prefetch.ptr(), prefetch.size(), loads);
	GS_NEED(loads.size() == 1);
	CHECK_FALSE(sim.res.commit_edit());
	GS_NEED(sim.res.begin_upload(loads[0]));
	GS_NEED(sim.res.finish_upload(loads[0], true));
	CHECK(sim.res.edit_ready());
	LocalVector<uint32_t> none;
	sim.res.edit_prefetch(none);
	CHECK(none.size() == 0);
	CHECK(sim.res.commit_edit());
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][Residency] a replacement child that can never load blocks the edit instead of publishing a hole") {
	EditSim sim;
	GS_NEED(sim.init(40));
	sim.step();
	sim.step();
	GS_NEED(sim.res.evict(3));
	sim.step();
	sim.step();
	sim.step();
	LocalVector<Residency::Ticket> tickets;
	const Residency::EditedLeaf leaf = { 9, 1000 };
	GS_NEED(sim.res.stage_edit(1, &leaf, 1, tickets) == Residency::EDIT_OK);
	GS_NEED(sim.res.finish_shadow(tickets[0], true));
	const uint32_t want[1] = { 3 };
	// Fail the load of node 3 until it is permanently failed.
	for (uint32_t attempt = 0; attempt < 3; ++attempt) {
		for (int i = 0; i < 16; ++i) { sim.step(); }
		LocalVector<Residency::Ticket> loads;
		sim.res.request(want, 1, loads);
		GS_NEED(loads.size() == 1);
		GS_NEED(sim.res.fail_load(loads[0]));
	}
	CHECK(sim.res.edit_blocked());
	CHECK_FALSE(sim.res.edit_ready());
	CHECK_FALSE(sim.res.commit_edit());
	CHECK(sim.res.resident_mask()[0] == 1); // Still the old, consistent cut.
	// A failed shadow payload blocks as well.
	GS_NEED(sim.res.cancel_edit());
	CHECK(sim.res.stage_edit(1, &leaf, 1, tickets) == Residency::EDIT_OK); // The reservation was given back.
}

// ---------------------------------------------------------------------------------------------------------------------
// ViewSet: node versions.
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][HLOD][Edit] the published draw record of an edited leaf carries a different NodeVersion than an untouched node") {
	GaussianSplatHlodRuntimeData edited;
	binary_tree(edited, 3);
	mark_edited(edited, { 4 }, 3);
	edited.source_revision = 7;
	CHECK(edited.node_content_revision(5) == 7);
	CHECK(edited.node_content_revision(4) != 7);
	CHECK(edited.node_content_revision(4) == ((uint64_t(1) << 63) | 3));
	GaussianSplatHlodRuntimeData control;
	binary_tree(control, 3);
	control.source_revision = 7;
	CHECK(control.node_content_revision(4) == 7);
}

// ---------------------------------------------------------------------------------------------------------------------
// Baked and paged worlds.
// ---------------------------------------------------------------------------------------------------------------------
static Ref<GaussianSplatWorld> make_world(uint32_t p_count, uint32_t p_high_order) {
	Ref<GaussianData> data;
	data.instantiate();
	LocalVector<Gaussian> gaussians;
	gaussians.resize(p_count);
	for (uint32_t i = 0; i < p_count; ++i) {
		Gaussian &g = gaussians[i];
		g.position = Vector3(float(i % 40) * 0.5f, float((i / 40) % 40) * 0.5f, float(i / 1600) * 0.5f);
		g.scale = Vector3(0.03125f, 0.0625f + float(i % 3) * 0.03125f, 0.125f);
		g.opacity = 0.5f;
		g.sh_dc = Color(0.25f + float(i % 4) * 0.125f, 0.5f, 0.75f, 1.0f);
		g.sh_1[0] = Vector3(0.125f, float(i % 5) * 0.0625f, 0.0f);
	}
	LocalVector<Vector3> sh;
	sh.resize(size_t(p_count) * p_high_order);
	for (uint32_t i = 0; i < sh.size(); ++i) {
		sh[i] = Vector3(float(i % 7) * 0.0078125f, 0.0625f, -0.015625f);
	}
	data->set_gaussian_payload(gaussians, sh, p_high_order > 0 ? 3u : 0u, p_high_order, false, false);
	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_gaussian_data(data);
	return world;
}

static String temp_path(const char *p_tag) {
	static int serial = 0;
	return OS::get_singleton()->get_temp_path().path_join(vformat("godotgs_f1_%s_%d_%d.gsplatworld", p_tag, ++serial, int64_t(OS::get_singleton()->get_ticks_usec())));
}

struct World {
	Ref<GaussianSplatWorld> world;
	Ref<GaussianSplatHlodRuntimeData> resident;
	Ref<GaussianSplatHlodRuntimeData> paged;
	Ref<GaussianSplatWorld> loaded; // Keeps the V4 source alive.
	String path;
};

static bool build_world(World &r_w, uint32_t p_high_order, bool p_paged, uint32_t p_count = 20000) {
	r_w.world = make_world(p_count, p_high_order);
	if (r_w.world->bake_hlod() != OK) { FAIL("bake failed"); return false; }
	String reason;
	if (r_w.world->capture_hlod_runtime_snapshot(r_w.resident, reason) != OK) { FAIL("resident snapshot: ", reason.utf8().get_data()); return false; }
	if (p_paged) {
		r_w.path = temp_path("paged");
		if (r_w.world->save_hlod_paged(r_w.path) != OK) { FAIL("save_hlod_paged failed"); return false; }
		Error load_error = ERR_BUG;
		ResourceFormatLoaderGaussianSplatWorld loader;
		r_w.loaded = loader.load(r_w.path, "", &load_error);
		if (load_error != OK || r_w.loaded.is_null() || r_w.loaded->capture_hlod_runtime_snapshot(r_w.paged, reason) != OK) {
			FAIL("paged snapshot failed");
			return false;
		}
	}
	return true;
}

// The first leaf of the snapshot's tree that has more than one splat.
static uint32_t pick_leaf(const GaussianSplatHlodRuntimeData &p_data) {
	for (uint32_t i = 0; i < uint32_t(p_data.nodes.size()); ++i) {
		if (p_data.nodes[i].topology.child_count == 0 && p_data.nodes[i].topology.payload_count > 4) { return i; }
	}
	return UINT32_MAX;
}

// The current payload of a leaf of a resident or paged snapshot, in the snapshot convention.
static bool read_leaf(const GaussianSplatHlodRuntimeData &p_data, uint32_t p_node, LeafEdit &r_edit) {
	r_edit.node = p_node;
	r_edit.gaussians.clear();
	r_edit.sh.clear();
	const auto &node = p_data.nodes[p_node].topology;
	if (p_data.is_paged()) {
		String why;
		return p_data.paged_source->read_node_payload(p_node, r_edit.gaussians, r_edit.sh, why) == OK;
	}
	for (uint32_t i = 0; i < node.payload_count; ++i) {
		r_edit.gaussians.push_back(p_data.gaussian(node.payload_first + i));
		const Vector3 *sh = p_data.sh(node.payload_first + i);
		for (uint32_t k = 0; sh && k < p_data.high_order_count; ++k) { r_edit.sh.push_back(sh[k]); }
	}
	return true;
}

// "Delete a box": keeps every other splat and recolours the rest (the same splats survive in the same order).
static LeafEdit delete_half(const LeafEdit &p_source, uint32_t p_high_order) {
	LeafEdit out;
	out.node = p_source.node;
	for (int64_t i = 0; i < p_source.gaussians.size(); i += 2) {
		Gaussian g = p_source.gaussians[i];
		g.sh_dc = Color(1.0f, 0.0f, 0.0f, 1.0f);
		out.gaussians.push_back(g);
		for (uint32_t k = 0; k < p_high_order; ++k) { out.sh.push_back(p_source.sh[i * p_high_order + k]); }
	}
	return out;
}

static void sha(const Vector<uint8_t> &p_bytes, uint8_t *r_out) {
	CryptoCore::SHA256Context ctx;
	ctx.start();
	if (p_bytes.size()) { ctx.update(p_bytes.ptr(), size_t(p_bytes.size())); }
	ctx.finish(r_out);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit] an edit derives a new snapshot: the leaf is replaced, exactly its ancestors are stale, the base is untouched") {
	World w;
	if (!build_world(w, 0, false, 120000)) { return; }
	const uint32_t leaf = pick_leaf(*w.resident.ptr());
	GS_NEED(leaf != UINT32_MAX);
	// The leaf needs at least two ancestors, or "all ancestors are stale" would not be told from "the parent is".
	GS_NEED(w.resident->nodes[w.resident->nodes[leaf].topology.parent].topology.parent != gs_hlod::kNoParent);
	LeafEdit source;
	GS_NEED(read_leaf(*w.resident.ptr(), leaf, source));
	const uint32_t old_count = w.resident->nodes[leaf].topology.payload_count;
	const LeafEdit edit = delete_half(source, 0);
	GS_NEED(uint32_t(edit.gaussians.size()) == (old_count + 1) / 2);

	Ref<GaussianSplatHlodRuntimeData> next;
	String reason;
	GS_NEED(make_edited_snapshot(w.resident, 0, &edit, 1, next, reason) == EditStatus::OK);
	GS_NEED(next.is_valid());
	CHECK(next->edit_revision == 1);
	CHECK(next->has_edits());
	CHECK(next->nodes[leaf].topology.payload_count == edit.gaussians.size());
	CHECK(next->node_edit_state(leaf) == GaussianSplatHlodRuntimeData::EDIT_LEAF);
	const GaussianSplatHlodRuntimeData::EditedLeaf *overlay = next->find_edited(leaf);
	GS_NEED(overlay != nullptr);
	GS_NEED(overlay->gaussians.size() == edit.gaussians.size());
	CHECK(overlay->gaussians[0].sh_dc == Color(1.0f, 0.0f, 0.0f, 1.0f));
	// Exactly the ancestors are stale, nothing else is touched.
	std::vector<uint8_t> expect_stale(next->nodes.size(), 0);
	for (uint32_t up = w.resident->nodes[leaf].topology.parent; up != gs_hlod::kNoParent; up = w.resident->nodes[up].topology.parent) { expect_stale[up] = 1; }
	uint32_t stale_count = 0;
	for (uint32_t i = 0; i < uint32_t(next->nodes.size()); ++i) {
		CHECK(next->is_stale(i) == (expect_stale[i] != 0));
		stale_count += next->is_stale(i) ? 1 : 0;
		if (i != leaf) {
			CHECK(next->nodes[i].topology.payload_count == w.resident->nodes[i].topology.payload_count);
			CHECK(next->node_content_revision(i) == w.resident->source_revision);
		}
	}
	CHECK(stale_count >= 1);
	CHECK(next->node_content_revision(leaf) != w.resident->source_revision);
	// The base snapshot and the world it came from are unchanged.
	CHECK(w.resident->edit_revision == 0);
	CHECK_FALSE(w.resident->has_edits());
	CHECK(w.resident->nodes[leaf].topology.payload_count == old_count);
	CHECK(w.resident->find_edited(leaf) == nullptr);
	CHECK(w.resident->gaussian(w.resident->nodes[leaf].topology.payload_first).sh_dc == source.gaussians[0].sh_dc);

	// An edit is made against a stated revision; a snapshot that moved on refuses the old one.
	Ref<GaussianSplatHlodRuntimeData> again;
	CHECK(make_edited_snapshot(next, 0, &edit, 1, again, reason) == EditStatus::STALE_REVISION);
	CHECK(again.is_null());
	GS_NEED(make_edited_snapshot(next, 1, &edit, 1, again, reason) == EditStatus::OK);
	CHECK(again->edit_revision == 2);
	CHECK(again->edited.size() == 1); // The same leaf edited again replaces its overlay.
	LocalVector<uint32_t> since;
	CHECK(collect_edited_since(*w.resident.ptr(), *next.ptr(), since));
	GS_NEED(since.size() == 1);
	CHECK(since[0] == leaf);
	CHECK_FALSE(collect_edited_since(*next.ptr(), *w.resident.ptr(), since));
}

TEST_CASE("[GaussianSplatting][HLOD][Edit] every refused edit names its own rule and leaves no snapshot") {
	World w;
	if (!build_world(w, 3, false)) { return; }
	const uint32_t leaf = pick_leaf(*w.resident.ptr());
	GS_NEED(leaf != UINT32_MAX);
	LeafEdit source;
	GS_NEED(read_leaf(*w.resident.ptr(), leaf, source));
	Ref<GaussianSplatHlodRuntimeData> out;
	String reason;
	auto refuse = [&](const LeafEdit *p_edits, uint32_t p_count, EditStatus p_expected, uint64_t p_revision = 0) {
		out.unref();
		const EditStatus status = make_edited_snapshot(w.resident, p_revision, p_edits, p_count, out, reason);
		CHECK(status == p_expected);
		CHECK(out.is_null());
		CHECK_FALSE(reason.is_empty());
	};
	const LeafEdit good = delete_half(source, 3);
	refuse(&good, 1, EditStatus::STALE_REVISION, 5);
	refuse(nullptr, 0, EditStatus::NO_EDITS);
	LeafEdit bad_node = good;
	bad_node.node = uint32_t(w.resident->nodes.size());
	refuse(&bad_node, 1, EditStatus::INVALID_NODE);
	const LeafEdit twice[2] = { good, good };
	refuse(twice, 2, EditStatus::INVALID_NODE);
	LeafEdit interior = good;
	interior.node = 0; // The root has children.
	refuse(&interior, 1, EditStatus::NOT_A_LEAF);
	LeafEdit empty = good;
	empty.gaussians.clear();
	empty.sh.clear();
	refuse(&empty, 1, EditStatus::EMPTY_LEAF);
	LeafEdit big = good;
	for (uint32_t i = 0; i < 16384; ++i) { big.gaussians.push_back(good.gaussians[0]); for (uint32_t k = 0; k < 3; ++k) { big.sh.push_back(good.sh[k]); } }
	refuse(&big, 1, EditStatus::LEAF_TOO_LARGE);
	LeafEdit sh_short = good;
	sh_short.sh.resize(sh_short.sh.size() - 1);
	refuse(&sh_short, 1, EditStatus::INVALID_PAYLOAD);
	LeafEdit nan_splat = good;
	nan_splat.gaussians.write[0].opacity = Math::NaN;
	refuse(&nan_splat, 1, EditStatus::INVALID_PAYLOAD);
	LeafEdit outside = good;
	outside.gaussians.write[0].position += Vector3(1000.0f, 0, 0); // Far outside the leaf's bounds: ancestors' bounds would be wrong.
	refuse(&outside, 1, EditStatus::INVALID_PAYLOAD);
	Vector<LeafEdit> many;
	for (uint32_t i = 0; i <= kMaxEditedLeaves; ++i) { many.push_back(good); }
	refuse(many.ptr(), many.size(), EditStatus::TOO_MANY_LEAVES);
	// A good edit still works on the same snapshot after all those refusals.
	CHECK(make_edited_snapshot(w.resident, 0, &good, 1, out, reason) == EditStatus::OK);
	CHECK(out.is_valid());
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][PagedV4] an edit coexists with a V4 package: the file is never written, unedited nodes keep their file payload") {
	World w;
	if (!build_world(w, 0, true)) { return; }
	GS_NEED(w.paged.is_valid() && w.paged->is_paged());
	const Vector<uint8_t> file_before = FileAccess::get_file_as_bytes(w.path);
	uint8_t digest_before[32];
	sha(file_before, digest_before);
	const uint32_t leaf = pick_leaf(*w.paged.ptr());
	GS_NEED(leaf != UINT32_MAX);
	LeafEdit source;
	GS_NEED(read_leaf(*w.paged.ptr(), leaf, source));
	const LeafEdit edit = delete_half(source, 0);
	Ref<GaussianSplatHlodRuntimeData> next;
	String reason;
	GS_NEED(make_edited_snapshot(w.paged, 0, &edit, 1, next, reason) == EditStatus::OK);
	CHECK(next->is_paged()); // Still reading unedited nodes from the file.
	CHECK(next->paged_source == w.paged->paged_source);
	CHECK(next->leaves.get_gaussians().is_empty());
	CHECK(next->find_edited(leaf) != nullptr);

	// The edited leaf is served from the overlay; every other node reads exactly its file record.
	String why;
	Vector<Gaussian> file_gaussians;
	Vector<Vector3> file_sh;
	for (uint32_t i = 0; i < uint32_t(next->nodes.size()); ++i) {
		if (next->nodes[i].topology.payload_count == 0) { continue; }
		const bool edited = i == leaf;
		CHECK((next->find_edited(i) != nullptr) == edited);
		if (!edited) {
			CHECK(next->paged_source->read_node_payload(i, file_gaussians, file_sh, why) == OK);
			CHECK(uint32_t(file_gaussians.size()) == next->nodes[i].topology.payload_count);
		}
	}
	// The recorded file record of the edited leaf still holds the OLD payload: the edit is an overlay, not a rewrite.
	GS_NEED(next->paged_source->read_node_payload(leaf, file_gaussians, file_sh, why) == OK);
	CHECK(uint32_t(file_gaussians.size()) == w.paged->nodes[leaf].topology.payload_count);
	CHECK(file_gaussians[0].sh_dc == source.gaussians[0].sh_dc);
	const Vector<uint8_t> file_after = FileAccess::get_file_as_bytes(w.path);
	uint8_t digest_after[32];
	sha(file_after, digest_after);
	CHECK(memcmp(digest_before, digest_after, 32) == 0);
	CHECK(file_before.size() == file_after.size());
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][PagedV4] the node streamer packs an edited leaf from the overlay, stages it without touching the served cut, and rejects what is not its derivative") {
	World w;
	if (!build_world(w, 0, true)) { return; }
	const uint32_t leaf = pick_leaf(*w.paged.ptr());
	GS_NEED(leaf != UINT32_MAX);
	LeafEdit source;
	GS_NEED(read_leaf(*w.paged.ptr(), leaf, source));
	const LeafEdit edit = delete_half(source, 0);
	Ref<GaussianSplatHlodRuntimeData> next;
	String reason;
	GS_NEED(make_edited_snapshot(w.paged, 0, &edit, 1, next, reason) == EditStatus::OK);

	HlodNodeStreamer::Config config;
	uint32_t pages = 0;
	for (int n = 0; n < w.paged->nodes.size(); ++n) {
		pages += (w.paged->nodes[n].topology.payload_count + config.page_splats - 1u) / config.page_splats;
	}
	config.atlas_pages = pages + 8;
	Ref<HlodNodeStreamer> streamer;
	streamer.instantiate();
	GS_NEED(streamer->init(w.paged, config, 1, 1));
	streamer->begin_frame();
	CHECK(streamer->get_data() == w.paged);

	// Not a derivative of the served snapshot: refused, nothing staged.
	CHECK(streamer->stage_edit(Ref<GaussianSplatHlodRuntimeData>()) == HlodNodeStreamer::EDIT_REJECTED_DATA);
	CHECK(streamer->stage_edit(w.paged) == HlodNodeStreamer::EDIT_REJECTED_DATA);
	Ref<GaussianSplatHlodRuntimeData> skip; // Revision 2 against a served revision 0.
	{
		Ref<GaussianSplatHlodRuntimeData> once;
		GS_NEED(make_edited_snapshot(w.paged, 0, &edit, 1, once, reason) == EditStatus::OK);
		GS_NEED(make_edited_snapshot(once, 1, &edit, 1, skip, reason) == EditStatus::OK);
	}
	CHECK(streamer->stage_edit(skip) == HlodNodeStreamer::EDIT_REJECTED_DATA);
	CHECK_FALSE(streamer->is_edit_staged());

	GS_NEED(streamer->stage_edit(next) == HlodNodeStreamer::EDIT_STAGED);
	CHECK(streamer->is_edit_staged());
	CHECK(streamer->stage_edit(next) == HlodNodeStreamer::EDIT_REJECTED_BUSY);
	// Staged only: the served snapshot is still the old one.
	CHECK(streamer->get_data() == w.paged);
	CHECK(streamer->get_data()->edit_revision == 0);

	// The shadow job packs the edited leaf. Without a device nothing uploads, which is irrelevant here: the packed
	// payload waits for the publication boundary.
	for (int frame = 0; frame < 600 && streamer->get_telemetry().edit_shadow_ready < 1; ++frame) {
		streamer->begin_frame();
		streamer->update(nullptr, HlodNodeStreamer::Buffers(), nullptr, 0, nullptr, 0);
		OS::get_singleton()->delay_usec(1000);
	}
	GS_NEED(streamer->get_telemetry().edit_shadow_ready == 1);
	CHECK(streamer->get_telemetry().edit_staged);
	Vector<PackedGaussian> packed;
	ChunkMetaGPU meta;
	GS_NEED(streamer->test_get_shadow_payload(leaf, packed, meta));
	CHECK(uint32_t(packed.size()) == edit.gaussians.size());
	CHECK(meta.splat_count == edit.gaussians.size());
	// Bit-identical to packing the replacement payload through the shared packing path.
	Vector<PackedGaussian> expect;
	expect.resize(edit.gaussians.size());
	ChunkMetaGPU expect_meta;
	SHCompressionMetrics metrics;
	gs_hlod_pack::Output out;
	out.raw = expect.ptrw();
	out.meta = &expect_meta;
	out.metrics = &metrics;
	gs_hlod_pack::Params params;
	params.relative = true;
	gs_hlod_pack::PayloadView view;
	view.gaussians = edit.gaussians.ptr();
	GS_NEED(gs_hlod_pack::pack_node(*next.ptr(), leaf, params, meta.atlas_base, out, &view));
	CHECK(memcmp(packed.ptr(), expect.ptr(), size_t(expect.size()) * sizeof(PackedGaussian)) == 0);
	CHECK(meta.atlas_base == expect_meta.atlas_base);
	// Cancelling gives the reservation back and leaves the served snapshot alone.
	CHECK(streamer->cancel_staged_edit());
	CHECK_FALSE(streamer->is_edit_staged());
	CHECK(streamer->get_data() == w.paged);
	CHECK(streamer->get_residency().stats().pages_shadow == 0);
	streamer->shutdown();
}


// ---------------------------------------------------------------------------------------------------------------------
// GPU: the publication boundary on a real RenderingDevice (needs a non-headless run with --gs-gpu-test).
// ---------------------------------------------------------------------------------------------------------------------
struct GpuEditRun {
	RenderingDevice *rd = nullptr;
	HlodNodeStreamer::Buffers buffers;
	uint64_t atlas_bytes = 0;
	~GpuEditRun() {
		if (rd) {
			if (buffers.atlas.is_valid()) { rd->free(buffers.atlas); }
			if (buffers.chunk_meta.is_valid()) { rd->free(buffers.chunk_meta); }
		}
	}
};

static void run_gpu_edit(const Ref<GaussianSplatHlodRuntimeData> &p_base, const char *p_label) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) { FAIL("GPU harness RenderingDevice required"); return; }
	const uint32_t node_total = p_base->nodes.size();
	HlodNodeStreamer::Config config;
	uint32_t pages = 0;
	for (uint32_t n = 0; n < node_total; ++n) {
		pages += (p_base->nodes[n].topology.payload_count + config.page_splats - 1u) / config.page_splats;
	}
	config.atlas_pages = pages + 16; // Room for the shadow run and the retiring runs.
	Ref<HlodNodeStreamer> streamer;
	streamer.instantiate();
	GS_NEED(streamer->init(p_base, config, 1, rd->get_device_instance_id()));
	GpuEditRun gpu;
	gpu.rd = rd;
	gpu.atlas_bytes = streamer->atlas_bytes_capacity();
	Vector<uint8_t> zero_atlas, zero_meta;
	zero_atlas.resize(int64_t(gpu.atlas_bytes));
	zero_atlas.fill(0);
	zero_meta.resize(int64_t(node_total) * int64_t(sizeof(ChunkMetaGPU)));
	zero_meta.fill(0);
	gpu.buffers.atlas = rd->storage_buffer_create(uint32_t(gpu.atlas_bytes), zero_atlas);
	gpu.buffers.chunk_meta = rd->storage_buffer_create(uint32_t(zero_meta.size()), zero_meta);
	GS_NEED(gpu.buffers.atlas.is_valid() && gpu.buffers.chunk_meta.is_valid());

	// Everything is demanded, so the whole tree streams in (children of one parent are consecutive in BFS order).
	LocalVector<DesiredRecord> everything;
	for (uint32_t n = 0; n < node_total; ++n) {
		DesiredRecord record;
		record.node = n;
		everything.push_back(record);
	}
	auto pump = [&](uint32_t p_frames, const std::function<bool()> &p_done) {
		for (uint32_t f = 0; f < p_frames && !p_done(); ++f) {
			streamer->begin_frame();
			streamer->commit_staged_edit(rd, gpu.buffers);
			streamer->update(rd, gpu.buffers, everything.ptr(), everything.size(), nullptr, 0);
			OS::get_singleton()->delay_usec(1000);
		}
	};
	pump(2000, [&]() { return streamer->get_telemetry().resident == node_total; });
	GS_NEED(streamer->get_telemetry().resident == node_total);
	CHECK(streamer->get_telemetry().failed_total == 0);

	const uint32_t leaf = pick_leaf(*p_base.ptr());
	GS_NEED(leaf != UINT32_MAX);
	LeafEdit source;
	GS_NEED(read_leaf(*p_base.ptr(), leaf, source));
	const LeafEdit edit = delete_half(source, p_base->high_order_count);
	Ref<GaussianSplatHlodRuntimeData> edited;
	String reason;
	GS_NEED(make_edited_snapshot(p_base, 0, &edit, 1, edited, reason) == EditStatus::OK);

	const Vector<uint8_t> meta_before = rd->buffer_get_data(gpu.buffers.chunk_meta);
	const Vector<uint8_t> atlas_before = rd->buffer_get_data(gpu.buffers.atlas);

	GS_NEED(streamer->stage_edit(edited) == HlodNodeStreamer::EDIT_STAGED);
	// Staged only: not one byte of GPU metadata changed and the served snapshot is the old one.
	CHECK(streamer->get_data() == p_base);
	const Vector<uint8_t> meta_staged = rd->buffer_get_data(gpu.buffers.chunk_meta);
	CHECK(meta_staged == meta_before);
	pump(2000, [&]() { return streamer->get_telemetry().edits_committed_total != 0; });
	GS_NEED(streamer->get_telemetry().edits_committed_total == 1);
	CHECK(streamer->get_telemetry().edit_commit_failures_total == 0);
	CHECK(streamer->get_data() == edited);
	CHECK_FALSE(streamer->is_edit_staged());

	const Vector<uint8_t> meta_after = rd->buffer_get_data(gpu.buffers.chunk_meta);
	const Vector<uint8_t> atlas_after = rd->buffer_get_data(gpu.buffers.atlas);
	const ChunkMetaGPU *before = reinterpret_cast<const ChunkMetaGPU *>(meta_before.ptr());
	const ChunkMetaGPU *after = reinterpret_cast<const ChunkMetaGPU *>(meta_after.ptr());
	uint32_t stale_total = 0;
	for (uint32_t n = 0; n < node_total; ++n) {
		if (edited->is_stale(n)) {
			// A stale ancestor can never be drawn again: its metadata is zeroed.
			++stale_total;
			CHECK(after[n].splat_count == 0);
			CHECK(streamer->resident_mask()[n] == 0);
		} else if (n == leaf) {
			CHECK(after[n].splat_count == edit.gaussians.size());
			CHECK(after[n].atlas_base != before[n].atlas_base); // On its new run.
			CHECK(streamer->resident_mask()[n] == 1);
		} else {
			// Unedited regions are byte-identical: metadata and atlas run.
			CHECK(memcmp(&before[n], &after[n], sizeof(ChunkMetaGPU)) == 0);
			uint32_t first = 0, run_pages = 0;
			if (streamer->get_residency().run(n, first, run_pages)) {
				const uint64_t stride = streamer->record_stride();
				const uint64_t offset = uint64_t(first) * config.page_splats * stride;
				const uint64_t bytes = uint64_t(edited->nodes[n].topology.payload_count) * stride;
				CHECK(memcmp(atlas_before.ptr() + offset, atlas_after.ptr() + offset, size_t(bytes)) == 0);
			}
		}
	}
	CHECK(stale_total >= 1);
	// The edited leaf's new run holds exactly the replacement payload as the shared packing path packs it.
	Vector<PackedGaussian> expect;
	expect.resize(edit.gaussians.size());
	ChunkMetaGPU expect_meta;
	SHCompressionMetrics metrics;
	gs_hlod_pack::Output out;
	out.raw = expect.ptrw();
	out.meta = &expect_meta;
	out.metrics = &metrics;
	gs_hlod_pack::Params params;
	params.relative = true;
	gs_hlod_pack::PayloadView view;
	view.gaussians = edit.gaussians.ptr();
	view.sh = edit.sh.is_empty() ? nullptr : edit.sh.ptr();
	GS_NEED(gs_hlod_pack::pack_node(*edited.ptr(), leaf, params, after[leaf].atlas_base, out, &view));
	CHECK(memcmp(atlas_after.ptr() + uint64_t(after[leaf].atlas_base) * sizeof(PackedGaussian), expect.ptr(), size_t(expect.size()) * sizeof(PackedGaussian)) == 0);

	// What the selector may now draw: a complete cut of the NEW snapshot over the streamer's resident mask, with no
	// stale node, every drawn node resident and carrying splats, at a far and at a near quality threshold.
	CutHistory history;
	GS_NEED(history.reserve(node_total));
	for (const double tau : { 1.0e9, 0.001 }) {
		CutRequest request = base_request(node_total);
		request.tau_pixels = tau;
		request.resident = streamer->resident_mask().ptr();
		request.resident_count = streamer->resident_mask().size();
		history.invalidate();
		const CutHistory::Result cut = history.select(*streamer->get_data().ptr(), request);
		CHECK(cut.status == CutHistory::COMPLETE);
		const std::vector<uint32_t> drawn = cut_of(history, cut);
		CHECK(exact_cover(*edited.ptr(), drawn));
		uint32_t nonresident_drawn = 0;
		for (uint32_t node : drawn) {
			CHECK_FALSE(edited->is_stale(node));
			nonresident_drawn += (streamer->resident_mask()[node] == 0 || after[node].splat_count == 0) ? 1 : 0;
		}
		CHECK(nonresident_drawn == 0);
		if (tau > 1.0) {
			CHECK(std::count(drawn.begin(), drawn.end(), leaf) == 1); // Far view: the edited leaf is drawn at full detail.
		}
	}
	CHECK(streamer->get_telemetry().failed_total == 0);
	streamer->shutdown();
	(void)p_label;
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][World][SceneTree][RequiresGPU] a committed edit flips the GPU metadata atomically for a streamed v3 world and a paged V4 world") {
	if (!RenderingDevice::get_singleton() || !SceneTree::get_singleton()) { FAIL("WorldSceneTree GPU harness required"); return; }
	World w;
	if (!build_world(w, 3, true)) { return; }
	run_gpu_edit(w.resident, "v3");
	run_gpu_edit(w.paged, "v4");
}

}
