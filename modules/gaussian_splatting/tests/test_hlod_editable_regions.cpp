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
#include "../renderer/gaussian_splat_renderer.h"
#include "core/config/project_settings.h"
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

// A view that sees the whole world from far away: the record estimate stays within the budget.
static CutRequest far_view(uint32_t p_nodes, double p_tau = 1.0) {
	CutRequest r = base_request(p_nodes);
	r.camera = Vector3(0, 0, 1000);
	r.tau_pixels = p_tau;
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
static Residency::Config residency_config(uint32_t p_pages, uint32_t p_retire = 2) {
	Residency::Config c;
	c.page_capacity = p_pages;
	c.retire_delay_frames = p_retire;
	c.max_requests_per_frame = 64;
	c.evictions_per_frame = 64;
	return c;
}

// A residency table with the whole 4-level tree resident (7 interiors x 1 page + 8 leaves x 2 pages = 23 pages).
struct EditSim {
	Ref<GaussianSplatHlodRuntimeData> data;
	Residency res;
	uint64_t frame = 0;
	uint32_t retire = 2;
	bool init(uint32_t p_pages) {
		data.instantiate();
		binary_tree(*data.ptr(), 4);
		Vector<Residency::NodeInfo> infos;
		if (!make_node_infos(*data.ptr(), infos) || !res.reset(infos, residency_config(p_pages, retire), 1, 1)) { return false; }
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
	// The commit cancelled that load, so its late completion is the harmless kind, not a loss.
	const uint64_t dropped = sim.res.stats().late_dropped_total;
	const uint64_t dropped_cancelled = sim.res.stats().late_dropped_cancelled_total;
	CHECK_FALSE(sim.res.begin_upload(old_tickets[0]));
	CHECK(sim.res.stats().late_dropped_cancelled_total == dropped_cancelled + 1);
	CHECK(sim.res.stats().late_dropped_total == dropped);
	// A second delivery of the same cancelled ticket is a duplicate: real loss.
	CHECK_FALSE(sim.res.finish_upload(old_tickets[0], true));
	CHECK(sim.res.stats().late_dropped_cancelled_total == dropped_cancelled + 1);
	CHECK(sim.res.stats().late_dropped_total == dropped + 1);
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
	CHECK(sim.res.stats().evicted_total == before.evicted_total); // A refused edit evicts nothing.
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
	const uint64_t dropped_before = sim.res.stats().late_dropped_total;
	const uint64_t dropped_cancelled_before = sim.res.stats().late_dropped_cancelled_total;
	CHECK_FALSE(sim.res.finish_shadow(tickets[0], true)); // The cancelled edit's late payload is dropped.
	CHECK(sim.res.stats().late_dropped_cancelled_total == dropped_cancelled_before + 1); // Harmless, not a loss.
	CHECK(sim.res.stats().late_dropped_total == dropped_before);
	CHECK_FALSE(sim.res.cancel_edit());
	sim.step();
	sim.step();
	CHECK_FALSE(sim.res.is_protected(3)); // The hold is gone with the edit.
	// After a cancel the same revision can be staged again. stage_edit appends to the ticket list, so start empty.
	tickets.clear();
	GS_NEED(sim.res.stage_edit(1, &small, 1, tickets) == Residency::EDIT_OK);
	GS_NEED(tickets.size() == 1);
	// A wanted shadow payload completing twice is discarded as real loss: that edit was never cancelled.
	CHECK(sim.res.finish_shadow(tickets[0], true));
	CHECK_FALSE(sim.res.finish_shadow(tickets[0], true));
	CHECK(sim.res.stats().late_dropped_total == dropped_before + 1);
	CHECK(sim.res.stats().late_dropped_cancelled_total == dropped_cancelled_before + 1);
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
	streamer->register_view(1, far_view(uint32_t(w.paged->nodes.size())));

	// Not a derivative of the served snapshot: refused, nothing staged.
	CHECK(streamer->stage_edit(Ref<GaussianSplatHlodRuntimeData>()) == HlodNodeStreamer::EDIT_REJECTED_DATA);
	CHECK(streamer->stage_edit(w.paged) == HlodNodeStreamer::EDIT_REJECTED_LINEAGE); // Not derived from the served snapshot.
	Ref<GaussianSplatHlodRuntimeData> skip; // Revision 2 against a served revision 0.
	{
		Ref<GaussianSplatHlodRuntimeData> once;
		GS_NEED(make_edited_snapshot(w.paged, 0, &edit, 1, once, reason) == EditStatus::OK);
		GS_NEED(make_edited_snapshot(once, 1, &edit, 1, skip, reason) == EditStatus::OK);
	}
	CHECK(streamer->stage_edit(skip) == HlodNodeStreamer::EDIT_REJECTED_LINEAGE);
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

// The first leaf whose chain of ancestors up to (excluding) the root differs from `p_other`'s first step, so an edit there
// is in a sibling branch of an edit at `p_other`.
static uint32_t pick_sibling_branch_leaf(const GaussianSplatHlodRuntimeData &p_data, uint32_t p_other) {
	auto top = [&](uint32_t n) {
		while (p_data.nodes[n].topology.parent != 0 && p_data.nodes[n].topology.parent != gs_hlod::kNoParent) { n = p_data.nodes[n].topology.parent; }
		return n;
	};
	const uint32_t other_top = top(p_other);
	for (uint32_t i = 0; i < uint32_t(p_data.nodes.size()); ++i) {
		if (p_data.nodes[i].topology.child_count == 0 && p_data.nodes[i].topology.payload_count > 4 && i != p_other && top(i) != other_top &&
				p_data.find_edited(i) == nullptr) {
			return i;
		}
	}
	return UINT32_MAX;
}

static void run_gpu_edit(const Ref<GaussianSplatHlodRuntimeData> &p_base, const char *p_label) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) { FAIL("GPU harness RenderingDevice required"); return; }
	const uint32_t node_total = p_base->nodes.size();
	HlodNodeStreamer::Config config;
	uint32_t pages = 0;
	for (uint32_t n = 0; n < node_total; ++n) {
		pages += (p_base->nodes[n].topology.payload_count + config.page_splats - 1u) / config.page_splats;
	}
	config.atlas_pages = pages + 24; // Room for the shadow runs and the retiring runs.
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
	streamer->register_view(1, far_view(node_total, 1.0e9));

	// Everything that is not stale is demanded, so the whole tree streams in (children of one parent are consecutive in
	// BFS order). A stale node is never requested.
	auto pump = [&](uint32_t p_frames, const std::function<bool()> &p_done) {
		LocalVector<DesiredRecord> everything;
		for (uint32_t f = 0; f < p_frames && !p_done(); ++f) {
			everything.clear();
			for (uint32_t n = 0; n < node_total; ++n) {
				if (streamer->get_data()->is_stale(n)) { continue; }
				DesiredRecord record;
				record.node = n;
				everything.push_back(record);
			}
			streamer->begin_frame(rd, &gpu.buffers); // Commits a ready staged edit before anything selects.
			if (streamer->get_last_commit_result() == HlodNodeStreamer::COMMIT_DONE) {
				// The publication boundary itself: the moment commit returns, before this frame's update(), no stale ancestor
				// can be drawn (its metadata is already zero) and the served snapshot is the edited one.
				const Vector<uint8_t> at_boundary = rd->buffer_get_data(gpu.buffers.chunk_meta);
				const ChunkMetaGPU *boundary = reinterpret_cast<const ChunkMetaGPU *>(at_boundary.ptr());
				for (uint32_t n = 0; n < node_total; ++n) {
					if (streamer->get_data()->is_stale(n)) { CHECK(boundary[n].splat_count == 0); }
				}
			}
			streamer->update(rd, gpu.buffers, everything.ptr(), everything.size(), nullptr, 0);
			OS::get_singleton()->delay_usec(1000);
		}
	};
	auto nonstale_resident = [&]() {
		uint32_t expected = 0;
		for (uint32_t n = 0; n < node_total; ++n) { expected += streamer->get_data()->is_stale(n) ? 0u : 1u; }
		return streamer->get_telemetry().resident == expected;
	};
	pump(3000, nonstale_resident);
	GS_NEED(nonstale_resident());
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

	// A view whose caps cannot hold the replacement cut: refused before anything changes (ADR 14.5 reservation).
	streamer->register_view(2, far_view(2, 1.0e9));
	CHECK(streamer->stage_edit(edited) == HlodNodeStreamer::EDIT_REJECTED_INFEASIBLE);
	CHECK_FALSE(streamer->is_edit_staged());
	CHECK(streamer->get_data() == p_base);
	CHECK(rd->buffer_get_data(gpu.buffers.chunk_meta) == meta_before);
	streamer->register_view(2, far_view(node_total, 1.0e9)); // Same key: now every known view holds the cut.

	GS_NEED(streamer->stage_edit(edited) == HlodNodeStreamer::EDIT_STAGED);
	// Staged only: not one byte of GPU metadata changed and the served snapshot is the old one.
	CHECK(streamer->get_data() == p_base);
	const Vector<uint8_t> meta_staged = rd->buffer_get_data(gpu.buffers.chunk_meta);
	CHECK(meta_staged == meta_before);
	pump(3000, [&]() { return streamer->get_telemetry().edits_committed_total != 0; });
	GS_NEED(streamer->get_telemetry().edits_committed_total == 1);
	CHECK(streamer->get_telemetry().edit_commit_failures_total == 0);
	CHECK(streamer->get_data() == edited);
	CHECK_FALSE(streamer->is_edit_staged());

	// Verifies one committed state against the base: stale metadata zero, edited leaves on their new runs holding the
	// replacement payload, every other node byte-identical to `p_meta_ref` / `p_atlas_ref`, and the cut the selector may
	// draw is complete, stale-free, resident and carries splats (nonresident_drawn == 0).
	auto verify = [&](const Ref<GaussianSplatHlodRuntimeData> &p_served, const std::vector<std::pair<uint32_t, const LeafEdit *>> &p_edits,
						  const Vector<uint8_t> &p_meta_ref, const Vector<uint8_t> &p_atlas_ref) {
		const Vector<uint8_t> meta_after = rd->buffer_get_data(gpu.buffers.chunk_meta);
		const Vector<uint8_t> atlas_after = rd->buffer_get_data(gpu.buffers.atlas);
		const ChunkMetaGPU *before = reinterpret_cast<const ChunkMetaGPU *>(p_meta_ref.ptr());
		const ChunkMetaGPU *after = reinterpret_cast<const ChunkMetaGPU *>(meta_after.ptr());
		uint32_t stale_total = 0;
		for (uint32_t n = 0; n < node_total; ++n) {
			const LeafEdit *edited_leaf = nullptr;
			for (const auto &e : p_edits) { if (e.first == n) { edited_leaf = e.second; } }
			if (p_served->is_stale(n)) {
				++stale_total;
				CHECK(after[n].splat_count == 0); // A stale ancestor can never be drawn again.
				CHECK(streamer->resident_mask()[n] == 0);
			} else if (edited_leaf) {
				CHECK(after[n].splat_count == edited_leaf->gaussians.size());
				CHECK(streamer->resident_mask()[n] == 1);
				Vector<PackedGaussian> expect;
				expect.resize(edited_leaf->gaussians.size());
				ChunkMetaGPU expect_meta;
				SHCompressionMetrics metrics;
				gs_hlod_pack::Output out;
				out.raw = expect.ptrw();
				out.meta = &expect_meta;
				out.metrics = &metrics;
				gs_hlod_pack::Params params;
				params.relative = true;
				gs_hlod_pack::PayloadView view;
				view.gaussians = edited_leaf->gaussians.ptr();
				view.sh = edited_leaf->sh.is_empty() ? nullptr : edited_leaf->sh.ptr();
				if (gs_hlod_pack::pack_node(*p_served.ptr(), n, params, after[n].atlas_base, out, &view)) {
					CHECK(memcmp(atlas_after.ptr() + uint64_t(after[n].atlas_base) * sizeof(PackedGaussian), expect.ptr(), size_t(expect.size()) * sizeof(PackedGaussian)) == 0);
				}
			} else if (before[n].splat_count != 0) {
				// Unedited regions are byte-identical: metadata and atlas run.
				CHECK(memcmp(&before[n], &after[n], sizeof(ChunkMetaGPU)) == 0);
				uint32_t first = 0, run_pages = 0;
				if (streamer->get_residency().run(n, first, run_pages)) {
					const uint64_t stride = streamer->record_stride();
					const uint64_t offset = uint64_t(first) * config.page_splats * stride;
					const uint64_t bytes = uint64_t(p_served->nodes[n].topology.payload_count) * stride;
					CHECK(memcmp(p_atlas_ref.ptr() + offset, atlas_after.ptr() + offset, size_t(bytes)) == 0);
				}
			}
		}
		CHECK(stale_total >= 1);
		CutHistory history;
		GS_NEED(history.reserve(node_total));
		for (const double tau : { 1.0e9, 0.001 }) {
			CutRequest request = far_view(node_total, tau);
			request.resident = streamer->resident_mask().ptr();
			request.resident_count = streamer->resident_mask().size();
			history.invalidate();
			const CutHistory::Result cut = history.select(*streamer->get_data().ptr(), request);
			CHECK(cut.status == CutHistory::COMPLETE);
			const std::vector<uint32_t> drawn = cut_of(history, cut);
			CHECK(exact_cover(*p_served.ptr(), drawn));
			uint32_t nonresident_drawn = 0;
			for (uint32_t node : drawn) {
				CHECK_FALSE(p_served->is_stale(node));
				nonresident_drawn += (streamer->resident_mask()[node] == 0 || after[node].splat_count == 0) ? 1 : 0;
			}
			CHECK(nonresident_drawn == 0);
			if (tau > 1.0) {
				for (const auto &e : p_edits) {
					CHECK(std::count(drawn.begin(), drawn.end(), e.first) == 1); // Far view: edited leaves are drawn at full detail.
				}
			}
		}
	};
	verify(edited, { { leaf, &edit } }, meta_before, atlas_before);

	// P1-1 on a device: a second edit in a SIBLING branch must commit too (it used to deadlock forever).
	const uint32_t leaf2 = pick_sibling_branch_leaf(*edited.ptr(), leaf);
	if (leaf2 != UINT32_MAX) {
		const Vector<uint8_t> meta_mid = rd->buffer_get_data(gpu.buffers.chunk_meta);
		const Vector<uint8_t> atlas_mid = rd->buffer_get_data(gpu.buffers.atlas);
		LeafEdit source2;
		GS_NEED(read_leaf(*edited.ptr(), leaf2, source2));
		const LeafEdit edit2 = delete_half(source2, p_base->high_order_count);
		Ref<GaussianSplatHlodRuntimeData> edited2;
		GS_NEED(make_edited_snapshot(edited, 1, &edit2, 1, edited2, reason) == EditStatus::OK);
		GS_NEED(streamer->stage_edit(edited2) == HlodNodeStreamer::EDIT_STAGED);
		pump(3000, [&]() { return streamer->get_telemetry().edits_committed_total >= 2 || !streamer->is_edit_staged(); });
		CHECK(streamer->get_telemetry().edits_committed_total == 2);
		CHECK(streamer->get_data() == edited2);
		verify(edited2, { { leaf, &edit }, { leaf2, &edit2 } }, meta_mid, atlas_mid);
	} else {
		FAIL("the fixture has no leaf in a sibling branch");
	}
	CHECK(streamer->get_telemetry().failed_total == 0);
	CHECK(streamer->get_telemetry().edit_commit_failures_total == 0);
	streamer->shutdown();
	(void)p_label;
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][World][SceneTree][RequiresGPU] a committed edit flips the GPU metadata atomically for a streamed v3 world and a paged V4 world, twice in sibling branches") {
	if (!RenderingDevice::get_singleton() || !SceneTree::get_singleton()) { FAIL("WorldSceneTree GPU harness required"); return; }
	World w;
	if (!build_world(w, 3, true, 120000)) { return; }
	run_gpu_edit(w.resident, "v3");
	run_gpu_edit(w.paged, "v4");
}

// ---------------------------------------------------------------------------------------------------------------------
// Fix round (review of 2a97c49d8e8..7f0031fcbc7).
// ---------------------------------------------------------------------------------------------------------------------

// P1-1: a second edit in a sibling branch used to deadlock: the committed-stale node of the first edit was required to be
// resident, but a stale node is never requested.
TEST_CASE("[GaussianSplatting][HLOD][Edit][Residency] a second edit in a sibling branch commits after the first (stale nodes are expand-only, never required resident)") {
	EditSim sim;
	GS_NEED(sim.init(60));
	LocalVector<Residency::Ticket> tickets;
	const Residency::EditedLeaf first = { 7, 1000 }; // Path 7 -> 3 -> 1 -> 0.
	GS_NEED(sim.res.stage_edit(1, &first, 1, tickets) == Residency::EDIT_OK);
	GS_NEED(tickets.size() == 1);
	GS_NEED(sim.res.finish_shadow(tickets[0], true));
	GS_NEED(sim.res.commit_edit());
	CHECK(sim.res.is_stale(1));
	CHECK(sim.res.state(1) == Residency::ABSENT);
	sim.step();
	// Edit 2 under the other subtree: node 1 is a child of the (already stale) root and must not be required.
	tickets.clear();
	const Residency::EditedLeaf second = { 14, 1000 }; // Path 14 -> 6 -> 2 -> 0.
	GS_NEED(sim.res.stage_edit(2, &second, 1, tickets) == Residency::EDIT_OK);
	GS_NEED(tickets.size() == 1);
	LocalVector<uint32_t> prefetch;
	sim.res.edit_prefetch(prefetch);
	CHECK(prefetch.size() == 0); // Everything required is resident already; node 1 is not among the demand.
	GS_NEED(sim.res.finish_shadow(tickets[0], true));
	CHECK(sim.res.edit_ready());
	CHECK_FALSE(sim.res.edit_blocked());
	GS_NEED(sim.res.commit_edit());
	CHECK(sim.res.committed_edit_revision() == 2);
	CHECK(sim.res.is_stale(2));
	CHECK(sim.res.is_stale(6));
	CHECK(sim.res.resident_mask()[7] == 1);
	CHECK(sim.res.resident_mask()[14] == 1);
	// A third edit is admitted: the model is not left staged.
	sim.step();
	tickets.clear();
	const Residency::EditedLeaf third = { 10, 1000 };
	CHECK(sim.res.stage_edit(3, &third, 1, tickets) == Residency::EDIT_OK);
}

// The selector side of the same scenario: after two edits in sibling branches the cut is still complete.
TEST_CASE("[GaussianSplatting][HLOD][Edit] two committed edits in sibling branches still give a complete cut without stale nodes") {
	GaussianSplatHlodRuntimeData edited;
	binary_tree(edited, 4);
	mark_edited(edited, { 7, 14 }, 2);
	CutHistory history;
	GS_NEED(history.reserve(15));
	CutRequest request = base_request(15);
	request.tau_pixels = 1.0e9;
	uint8_t resident[15];
	for (int i = 0; i < 15; ++i) { resident[i] = 1; }
	for (uint32_t stale : { 0u, 1u, 2u, 3u, 6u }) { resident[stale] = 0; }
	request.resident = resident;
	request.resident_count = 15;
	const auto cut = history.select(edited, request);
	GS_NEED(cut.status == CutHistory::COMPLETE);
	CHECK(cut_of(history, cut) == (std::vector<uint32_t>{ 4, 5, 7, 8, 13, 14 }));
}

// P2-1: admission decides from free + reclaimable pages WITHOUT evicting; the waiting edit evicts only through
// advance_edit(), after the frame's protection is known, and survives a retirement delay of 3.
TEST_CASE("[GaussianSplatting][HLOD][Edit][Residency] admission never evicts; a waiting edit frees unprotected nodes at retire delay 3 and keeps the drawn cut") {
	EditSim sim;
	sim.retire = 3;
	GS_NEED(sim.init(24)); // 23 pages resident, 1 free.
	sim.step();
	sim.step();
	sim.step();
	sim.step();
	const Residency::Stats before = sim.res.stats();
	GS_NEED(before.pages_free == 1);
	LocalVector<Residency::Ticket> tickets;
	const Residency::EditedLeaf leaf = { 9, 2500 }; // 3 pages: needs 2 more than are free.
	GS_NEED(sim.res.stage_edit(1, &leaf, 1, tickets) == Residency::EDIT_OK);
	// Admitted without touching anyone: no eviction, nothing allocated, waiting for space.
	CHECK(tickets.size() == 0);
	CHECK(sim.res.stats().evicted_total == before.evicted_total);
	CHECK(sim.res.stats().pages_free == before.pages_free);
	CHECK(sim.res.stats().edit_waiting_for_space);
	CHECK_FALSE(sim.res.edit_ready());
	// The frames that follow: the drawn cut {2, 3, 9, 10} (and its ancestors) is protected, so only other nodes are freed.
	const uint32_t drawn[4] = { 2, 3, 9, 10 };
	for (int frame = 0; frame < 24 && tickets.size() == 0; ++frame) {
		sim.step();
		sim.res.protect_drawn(drawn, 4);
		sim.res.advance_edit(tickets);
	}
	GS_NEED(tickets.size() == 1);
	CHECK(tickets[0].shadow);
	CHECK(tickets[0].page_count == 3);
	CHECK(sim.res.stats().evicted_total > before.evicted_total);
	for (uint32_t keep : { 0u, 1u, 2u, 3u, 4u, 9u, 10u }) {
		CHECK(sim.res.state(keep) == Residency::RESIDENT); // The drawn cut and its ancestors were never evicted.
	}
	CHECK_FALSE(sim.res.stats().edit_waiting_for_space);
	// Truly infeasible: nothing is evicted and nothing changes.
	GS_NEED(sim.res.cancel_edit());
	sim.step();
	sim.res.protect_drawn(drawn, 4);
	const uint64_t evicted = sim.res.stats().evicted_total;
	const uint32_t free_pages = sim.res.stats().pages_free;
	const Residency::EditedLeaf huge = { 9, 1000000 };
	LocalVector<Residency::Ticket> none;
	CHECK(sim.res.stage_edit(1, &huge, 1, none) == Residency::EDIT_NO_SPACE);
	CHECK(sim.res.stats().evicted_total == evicted);
	CHECK(sim.res.stats().pages_free == free_pages);
	CHECK_FALSE(sim.res.is_edit_staged());
}

// P3: the stale-in-cut guard is reachable only through an inconsistent edit state; build one by hand.
TEST_CASE("[GaussianSplatting][HLOD][Edit] a stale node that the quality rule selects is refused as INVALID, never published") {
	GaussianSplatHlodRuntimeData data;
	binary_tree(data, 4);
	data.edit_revision = 1;
	data.edit_state.resize(15);
	data.edit_state.fill(GaussianSplatHlodRuntimeData::EDIT_CLEAN);
	data.edit_state.write[1] = GaussianSplatHlodRuntimeData::EDIT_STALE; // Inconsistent: its ancestor, the root, is not stale.
	CutHistory history;
	GS_NEED(history.reserve(15));
	CutRequest request = base_request(15);
	request.camera = Vector3(0, 0, 1000);
	request.tau_pixels = 0.5; // Refines the root, keeps its children.
	const auto cut = history.select(data, request);
	CHECK(cut.status == CutHistory::INVALID);
	CHECK(cut.selected_count == 0);
}

// ViewSet: the forced replacement is visible in the view result, with and without a complete cut.
TEST_CASE("[GaussianSplatting][HLOD][Edit] a snapshot can only be derived from the one being served, and an edited snapshot initializes a streamer with its stale nodes") {
	World w;
	if (!build_world(w, 0, true)) { return; }
	const uint32_t leaf = pick_leaf(*w.paged.ptr());
	GS_NEED(leaf != UINT32_MAX);
	LeafEdit source;
	GS_NEED(read_leaf(*w.paged.ptr(), leaf, source));
	const LeafEdit edit = delete_half(source, 0);
	Ref<GaussianSplatHlodRuntimeData> one, other, next_of_other;
	String reason;
	GS_NEED(make_edited_snapshot(w.paged, 0, &edit, 1, one, reason) == EditStatus::OK);
	GS_NEED(make_edited_snapshot(w.paged, 0, &edit, 1, other, reason) == EditStatus::OK);
	GS_NEED(make_edited_snapshot(other, 1, &edit, 1, next_of_other, reason) == EditStatus::OK);
	CHECK(next_of_other->edit_revision == 2);
	CHECK(next_of_other->edit_parent_id == other->get_instance_id());
	CHECK(next_of_other->edit_parent_id != one->get_instance_id());

	HlodNodeStreamer::Config config;
	uint32_t pages = 0;
	for (int n = 0; n < w.paged->nodes.size(); ++n) {
		pages += (w.paged->nodes[n].topology.payload_count + config.page_splats - 1u) / config.page_splats;
	}
	config.atlas_pages = pages + 8;
	Ref<HlodNodeStreamer> streamer;
	streamer.instantiate();
	// The streamer serves `one` (revision 1): its residency starts with the stale ancestors dropped and the committed revision.
	GS_NEED(streamer->init(one, config, 1, 1));
	CHECK(streamer->get_residency().committed_edit_revision() == 1);
	CHECK(streamer->get_residency().is_stale(0));
	CHECK_FALSE(streamer->get_residency().is_stale(leaf));
	CHECK(streamer->get_telemetry().edit_revision == 1);
	streamer->begin_frame();
	CutRequest view = far_view(uint32_t(one->nodes.size()));
	streamer->register_view(1, view);
	// Revision 2 of ANOTHER lineage: the revision number alone is not enough.
	CHECK(streamer->stage_edit(next_of_other) == HlodNodeStreamer::EDIT_REJECTED_LINEAGE);
	CHECK_FALSE(streamer->is_edit_staged());
	Ref<GaussianSplatHlodRuntimeData> next_of_one;
	GS_NEED(make_edited_snapshot(one, 1, &edit, 1, next_of_one, reason) == EditStatus::OK);
	CHECK(streamer->stage_edit(next_of_one) == HlodNodeStreamer::EDIT_STAGED);
	streamer->shutdown();
}

// P1-3: the replacement cut must fit the caps of the views the streamer knows, or the edit is refused before any change.
TEST_CASE("[GaussianSplatting][HLOD][Edit][PagedV4] an edit whose replacement cut does not fit a registered view is refused with nothing changed") {
	World w;
	if (!build_world(w, 0, true, 120000)) { return; }
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
	// No view known: feasibility cannot be shown.
	CHECK(streamer->stage_edit(next) == HlodNodeStreamer::EDIT_REJECTED_NO_VIEW);
	// A view whose node budget cannot hold the replacement cut (it needs the path's siblings plus the leaf).
	CutRequest tiny = far_view(2, 1.0e9);
	streamer->register_view(7, tiny);
	CHECK(streamer->stage_edit(next) == HlodNodeStreamer::EDIT_REJECTED_INFEASIBLE);
	CHECK_FALSE(streamer->is_edit_staged());
	CHECK(streamer->get_residency().stats().pages_shadow == 0);
	CHECK(streamer->get_telemetry().edits_rejected_infeasible_total == 1);
	CHECK(streamer->get_data() == w.paged);
	// A second, roomy view does not rescue the tiny one: EVERY known view must hold the cut.
	CutRequest roomy = far_view(uint32_t(w.paged->nodes.size()), 1.0e9);
	streamer->register_view(8, roomy);
	CHECK(streamer->stage_edit(next) == HlodNodeStreamer::EDIT_REJECTED_INFEASIBLE);
	// Replace the tiny view by a roomy one under the same key: now it fits.
	streamer->register_view(7, roomy);
	CHECK(streamer->stage_edit(next) == HlodNodeStreamer::EDIT_STAGED);
	streamer->shutdown();
}

// Engine item 4: a staged edit that never commits is cancelled and reported instead of holding its reservation forever.
TEST_CASE("[GaussianSplatting][HLOD][Edit][PagedV4] a staged edit that never commits times out, gives its pages back and says why") {
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
	config.edit_timeout_frames = 3;
	Ref<HlodNodeStreamer> streamer;
	streamer.instantiate();
	GS_NEED(streamer->init(w.paged, config, 1, 1));
	streamer->begin_frame();
	streamer->register_view(1, far_view(uint32_t(w.paged->nodes.size())));
	GS_NEED(streamer->stage_edit(next) == HlodNodeStreamer::EDIT_STAGED);
	// No device: the replacement children can never upload, so the edit can never commit.
	for (int frame = 0; frame < 8; ++frame) {
		streamer->begin_frame();
		streamer->update(nullptr, HlodNodeStreamer::Buffers(), nullptr, 0, nullptr, 0);
	}
	CHECK_FALSE(streamer->is_edit_staged());
	CHECK(streamer->get_telemetry().edit_timeouts_total == 1);
	CHECK(String(streamer->get_telemetry().edit_event) == String("timeout"));
	CHECK(streamer->get_residency().stats().pages_shadow == 0);
	CHECK(streamer->get_data() == w.paged);
	streamer->shutdown();
}

// P2-4: other consumers of an edited snapshot must not read the OLD splats through the base ranges.
TEST_CASE("[GaussianSplatting][HLOD][Edit] an edited snapshot refuses raw payload indices of an edited leaf and packs the leaf from its overlay on every path") {
	World w;
	if (!build_world(w, 3, false)) { return; }
	const uint32_t leaf = pick_leaf(*w.resident.ptr());
	GS_NEED(leaf != UINT32_MAX);
	LeafEdit source;
	GS_NEED(read_leaf(*w.resident.ptr(), leaf, source));
	const LeafEdit edit = delete_half(source, 3);
	Ref<GaussianSplatHlodRuntimeData> next;
	String reason;
	GS_NEED(make_edited_snapshot(w.resident, 0, &edit, 1, next, reason) == EditStatus::OK);
	const uint64_t first = w.resident->nodes[leaf].topology.payload_first;
	const uint32_t old_count = w.resident->nodes[leaf].topology.payload_count;
	GS_NEED(next->nodes[leaf].topology.payload_count < old_count);
	CHECK(next->index_in_edited_leaf(first));
	CHECK(next->index_in_edited_leaf(first + old_count - 1));
	CHECK_FALSE(next->index_in_edited_leaf(first + old_count));
	ERR_PRINT_OFF;
	const Gaussian &refused = next->gaussian(first);
	const Vector3 *refused_sh = next->sh(first);
	ERR_PRINT_ON;
	CHECK(refused.sh_dc == Color()); // The placeholder, not the old splat.
	CHECK(refused_sh == nullptr);
	CHECK(next->gaussian(first + old_count).opacity == w.resident->gaussian(first + old_count).opacity); // Outside the leaf: served.
	// pack_node without an explicit view packs the overlay (the whole-bundle builder and every other caller).
	gs_hlod_pack::Params params;
	params.relative = true;
	const uint32_t count = next->nodes[leaf].topology.payload_count;
	Vector<PackedGaussian> implicit_pack, explicit_pack;
	implicit_pack.resize(count);
	explicit_pack.resize(count);
	ChunkMetaGPU meta_a, meta_b;
	SHCompressionMetrics metrics;
	gs_hlod_pack::Output out_a, out_b;
	out_a.raw = implicit_pack.ptrw();
	out_a.meta = &meta_a;
	out_a.metrics = &metrics;
	out_b.raw = explicit_pack.ptrw();
	out_b.meta = &meta_b;
	out_b.metrics = &metrics;
	gs_hlod_pack::PayloadView view;
	view.gaussians = edit.gaussians.ptr();
	view.sh = edit.sh.ptr();
	GS_NEED(gs_hlod_pack::pack_node(*next.ptr(), leaf, params, 0, out_a));
	GS_NEED(gs_hlod_pack::pack_node(*next.ptr(), leaf, params, 0, out_b, &view));
	CHECK(memcmp(implicit_pack.ptr(), explicit_pack.ptr(), size_t(count) * sizeof(PackedGaussian)) == 0);
	CHECK(meta_a.splat_count == count);
	// An unedited leaf is untouched by the overlay logic.
	for (uint32_t n = 0; n < uint32_t(next->nodes.size()); ++n) {
		if (n == leaf || next->nodes[n].topology.child_count != 0) { continue; }
		Vector<PackedGaussian> base_pack, edited_pack;
		const uint32_t c = next->nodes[n].topology.payload_count;
		base_pack.resize(c);
		edited_pack.resize(c);
		ChunkMetaGPU ma, mb;
		gs_hlod_pack::Output oa, ob;
		oa.raw = base_pack.ptrw();
		oa.meta = &ma;
		oa.metrics = &metrics;
		ob.raw = edited_pack.ptrw();
		ob.meta = &mb;
		ob.metrics = &metrics;
		GS_NEED(gs_hlod_pack::pack_node(*w.resident.ptr(), n, params, 0, oa));
		GS_NEED(gs_hlod_pack::pack_node(*next.ptr(), n, params, 0, ob));
		CHECK(memcmp(base_pack.ptr(), edited_pack.ptr(), size_t(c) * sizeof(PackedGaussian)) == 0);
		break;
	}
}

// P1-2: once edits are committed, a route that cannot serve them draws nothing, with a reason.
TEST_CASE("[GaussianSplatting][HLOD][Edit] a world with committed edits blanks every frame the streamed route cannot serve, and only that world") {
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	GS_NEED(renderer.is_valid());
	auto &state = renderer->get_resource_state();
	String reason;
	CHECK_FALSE(renderer->hlod_edit_blanks_frame(reason)); // No edit: the canonical route is fine.
	Ref<GaussianSplatHlodRuntimeData> edited;
	edited.instantiate();
	edited->edit_revision = 1;
	state.hlod_committed_edit = edited;
	state.hlod_edit_owner_key = 77;
	state.hlod_owner_key = 77;
	state.hlod_reason = "lod_disabled_canonical_leaves";
	CHECK(renderer->hlod_edit_blanks_frame(reason)); // LOD off, materialized resident copy, released route: all redisplay old content.
	CHECK(reason.begins_with("hlod_edit_route_unavailable"));
	CHECK(reason.contains("lod_disabled_canonical_leaves"));
	state.hlod_owner_key = 78; // A different world is prepared: the old world's edit does not blank it.
	CHECK_FALSE(renderer->hlod_edit_blanks_frame(reason));
	state.hlod_owner_key = 77;
	state.hlod_committed_edit.unref();
	CHECK_FALSE(renderer->hlod_edit_blanks_frame(reason));
	// The internal stage hook refuses without a streamed route, with a reason, and changes nothing.
	CHECK_FALSE(renderer->stage_hlod_edit(edited, reason));
	CHECK(reason == String("hlod_edit_requires_streamed_route"));
}


// ---------------------------------------------------------------------------------------------------------------------
// Second fix round: the engine cycle (publish / release / rebuild / blank), world identity and rebase.
// ---------------------------------------------------------------------------------------------------------------------

// Builds a streamer over `p_served` (no device: CPU only) and installs it, with a committed edit, in a renderer's state.
static Ref<HlodNodeStreamer> install_streamer(GaussianSplatRenderer &r_renderer, const Ref<GaussianSplatHlodRuntimeData> &p_served,
		const Ref<GaussianSplatHlodRuntimeData> &p_committed) {
	HlodNodeStreamer::Config config;
	uint32_t pages = 0;
	for (int n = 0; n < p_served->nodes.size(); ++n) {
		pages += (p_served->nodes[n].topology.payload_count + config.page_splats - 1u) / config.page_splats;
	}
	config.atlas_pages = pages + 8;
	Ref<HlodNodeStreamer> streamer;
	streamer.instantiate();
	if (!streamer->init(p_served, config, 1, 1)) {
		return Ref<HlodNodeStreamer>();
	}
	auto &state = r_renderer.get_resource_state();
	state.hlod_streaming = true;
	state.hlod_streamer = streamer;
	state.hlod_committed_edit = p_committed;
	state.hlod_owner_key = 5;
	state.hlod_edit_owner_key = 5;
	state.hlod_world_key = 6;
	state.hlod_edit_world_key = 6;
	return streamer;
}

// N-P1 / N-P2a: the gate is a function of the ROUTE STATE after the publish attempt: a route that serves the committed
// snapshot draws; a route that is gone, or that was rebuilt from the unedited snapshot, blanks.
TEST_CASE("[GaussianSplatting][HLOD][Edit] after the publish attempt an edited world draws only through a route that serves the committed snapshot") {
	World w;
	if (!build_world(w, 0, false)) { return; }
	const uint32_t leaf = pick_leaf(*w.resident.ptr());
	GS_NEED(leaf != UINT32_MAX);
	LeafEdit source;
	GS_NEED(read_leaf(*w.resident.ptr(), leaf, source));
	const LeafEdit edit = delete_half(source, 0);
	Ref<GaussianSplatHlodRuntimeData> edited;
	String reason;
	GS_NEED(make_edited_snapshot(w.resident, 0, &edit, 1, edited, reason) == EditStatus::OK);
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	GS_NEED(renderer.is_valid());
	auto &state = renderer->get_resource_state();

	// A streamer that serves the committed edit: drawn, not blanked.
	Ref<HlodNodeStreamer> serving = install_streamer(*renderer.ptr(), edited, edited);
	GS_NEED(serving.is_valid());
	CHECK(renderer->hlod_edit_committed_here());
	CHECK(renderer->hlod_edit_served());
	CHECK_FALSE(renderer->hlod_edit_blanks_frame(reason));

	// The streamer was rebuilt from the UNEDITED snapshot (what the old early gate allowed): it is alive, but it does not
	// serve the committed edit. That is the scenario that used to redisplay the old content: it must blank.
	Ref<HlodNodeStreamer> unedited = install_streamer(*renderer.ptr(), w.resident, edited);
	GS_NEED(unedited.is_valid());
	CHECK_FALSE(renderer->hlod_edit_served());
	CHECK(renderer->hlod_edit_blanks_frame(reason));
	CHECK(reason.begins_with("hlod_edit_route_unavailable"));
	// The blank reason of one frame is the stats reason of the next: it must not grow by wrapping itself every frame.
	state.hlod_reason = reason;
	String next_frame;
	CHECK(renderer->hlod_edit_blanks_frame(next_frame));
	CHECK(next_frame == reason);

	// The route was released (cut toggled off, lost submission, failed rebuild): nothing alive, blank, committed edit kept
	// so that the publish attempt can rebuild it.
	state.hlod_streamer->shutdown();
	CHECK(renderer->hlod_edit_blanks_frame(reason));
	state.hlod_streamer.unref();
	state.hlod_streaming = false;
	CHECK(renderer->hlod_edit_blanks_frame(reason));
	CHECK(state.hlod_committed_edit.is_valid()); // The rebuild source survives the release.

	// A world without edits is never blanked.
	state.hlod_committed_edit.unref();
	CHECK_FALSE(renderer->hlod_edit_blanks_frame(reason));
}

// N-P2c: an edited world is never routed to the legacy streaming backend, even while its HLOD streamer is alive.
TEST_CASE("[GaussianSplatting][HLOD][Edit] the backend plan of an edited world is forced to the resident route") {
	GaussianSplatRenderer::FrameBackendPlan plan;
	plan.prefer_resident_backend = false; // The legacy streaming backend would be chosen.
	GaussianSplatRenderer::apply_hlod_edit_backend_override(false, plan);
	CHECK_FALSE(plan.prefer_resident_backend); // No edits: the plan is untouched.
	GaussianSplatRenderer::apply_hlod_edit_backend_override(true, plan);
	CHECK(plan.prefer_resident_backend);
}

// N-P2b: the committed snapshot is released on a world swap, an owner change and a contract clear.
TEST_CASE("[GaussianSplatting][HLOD][Edit] the committed snapshot is released on world swap, owner change and contract clear, and frees the world and its V4 package") {
	ObjectID world_id, source_id;
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	GS_NEED(renderer.is_valid());
	auto &state = renderer->get_resource_state();
	{
		World w;
		if (!build_world(w, 0, true)) { return; }
		const uint32_t leaf = pick_leaf(*w.paged.ptr());
		GS_NEED(leaf != UINT32_MAX);
		LeafEdit source;
		GS_NEED(read_leaf(*w.paged.ptr(), leaf, source));
		const LeafEdit edit = delete_half(source, 0);
		Ref<GaussianSplatHlodRuntimeData> edited;
		String reason;
		GS_NEED(make_edited_snapshot(w.paged, 0, &edit, 1, edited, reason) == EditStatus::OK);
		world_id = w.loaded->get_instance_id();
		source_id = w.paged->paged_source->get_instance_id();
		state.hlod_committed_edit = edited;
		state.hlod_owner_key = 5;
		state.hlod_edit_owner_key = 5;
		state.hlod_world_key = 6;
		state.hlod_edit_world_key = 6;
	}
	// The test scope is gone: only the renderer's committed snapshot keeps the world and its package alive.
	CHECK(ObjectDB::get_instance(world_id) != nullptr);
	CHECK(ObjectDB::get_instance(source_id) != nullptr);
	// Same node, same world: kept (a toggle or a re-submission of the same world must still find its edit).
	renderer->reconcile_hlod_committed_edit();
	CHECK(state.hlod_committed_edit.is_valid());
	// The same node gets a DIFFERENT world (its owner key is unchanged): released.
	state.hlod_world_key = 7;
	renderer->reconcile_hlod_committed_edit();
	CHECK(state.hlod_committed_edit.is_null());
	CHECK(ObjectDB::get_instance(world_id) == nullptr);
	CHECK(ObjectDB::get_instance(source_id) == nullptr);

	// Another node submits the world: released as well.
	Ref<GaussianSplatHlodRuntimeData> marker;
	marker.instantiate();
	marker->edit_revision = 1;
	state.hlod_committed_edit = marker;
	state.hlod_owner_key = 5;
	state.hlod_edit_owner_key = 5;
	state.hlod_world_key = 6;
	state.hlod_edit_world_key = 6;
	state.hlod_owner_key = 9;
	renderer->reconcile_hlod_committed_edit();
	CHECK(state.hlod_committed_edit.is_null());

	// Freeing the node clears the world contract: the edit is released even though the keys still match.
	state.hlod_committed_edit = marker;
	state.hlod_owner_key = 5;
	state.hlod_edit_owner_key = 5;
	state.hlod_world_key = 6;
	state.hlod_edit_world_key = 6;
	renderer->clear_world_submission_contract();
	CHECK(state.hlod_committed_edit.is_null());
	CHECK(state.hlod_edit_owner_key == 0);
}

// The rebuild after a toggle: a V4 world that was materialized into the resident shape gets its committed edits back.
TEST_CASE("[GaussianSplatting][HLOD][Edit][PagedV4] committed edits are rebased onto a fresh capture of the same world, and onto nothing else") {
	World w;
	if (!build_world(w, 0, true, 120000)) { return; }
	const uint32_t leaf = pick_leaf(*w.paged.ptr());
	GS_NEED(leaf != UINT32_MAX);
	LeafEdit source;
	GS_NEED(read_leaf(*w.paged.ptr(), leaf, source));
	const LeafEdit edit = delete_half(source, 0);
	Ref<GaussianSplatHlodRuntimeData> edited;
	String reason;
	GS_NEED(make_edited_snapshot(w.paged, 0, &edit, 1, edited, reason) == EditStatus::OK);
	// The cut was toggled off: the node materialized the V4 world into the resident shape, and a later capture is resident.
	GS_NEED(w.loaded->materialize_paged_resident(reason) == OK);
	Ref<GaussianSplatHlodRuntimeData> resident;
	GS_NEED(w.loaded->capture_hlod_runtime_snapshot(resident, reason) == OK);
	GS_NEED(resident.is_valid() && !resident->is_paged());
	Ref<GaussianSplatHlodRuntimeData> rebased;
	GS_NEED(rebase_edited_snapshot(edited, resident, rebased));
	CHECK_FALSE(rebased->is_paged());
	CHECK(rebased->edit_revision == 1);
	CHECK(rebased->find_edited(leaf) != nullptr);
	CHECK(rebased->nodes[leaf].topology.payload_count == edit.gaussians.size());
	CHECK(rebased->is_stale(0));
	CHECK(rebased->source_world == resident->source_world);
	// The overlay is laid over the resident payload: the edited leaf packs the replacement, an untouched leaf packs the
	// resident splats, bit-identical to what the paged snapshot reads from the file.
	gs_hlod_pack::Params params;
	params.relative = true;
	auto pack = [&](const GaussianSplatHlodRuntimeData &p_data, uint32_t p_node, Vector<PackedGaussian> &r_out, const gs_hlod_pack::PayloadView *p_view) {
		r_out.resize(p_data.nodes[p_node].topology.payload_count);
		ChunkMetaGPU meta;
		SHCompressionMetrics metrics;
		gs_hlod_pack::Output out;
		out.raw = r_out.ptrw();
		out.meta = &meta;
		out.metrics = &metrics;
		return gs_hlod_pack::pack_node(p_data, p_node, params, 0, out, p_view);
	};
	Vector<PackedGaussian> a, b;
	GS_NEED(pack(*rebased.ptr(), leaf, a, nullptr));
	gs_hlod_pack::PayloadView view;
	view.gaussians = edit.gaussians.ptr();
	GS_NEED(pack(*edited.ptr(), leaf, b, &view));
	CHECK(a.size() == b.size());
	CHECK(memcmp(a.ptr(), b.ptr(), size_t(a.size()) * sizeof(PackedGaussian)) == 0);
	for (uint32_t n = 0; n < uint32_t(resident->nodes.size()); ++n) {
		if (n == leaf || resident->nodes[n].topology.child_count != 0) { continue; }
		Vector<PackedGaussian> from_resident, from_file;
		Vector<Gaussian> g;
		Vector<Vector3> sh;
		String why;
		GS_NEED(w.paged->paged_source->read_node_payload(n, g, sh, why) == OK);
		gs_hlod_pack::PayloadView file_view;
		file_view.gaussians = g.ptr();
		GS_NEED(pack(*rebased.ptr(), n, from_resident, nullptr));
		GS_NEED(pack(*w.paged.ptr(), n, from_file, &file_view));
		CHECK(memcmp(from_resident.ptr(), from_file.ptr(), size_t(from_file.size()) * sizeof(PackedGaussian)) == 0);
		break;
	}
	// A different world with a similar tree, a snapshot that is already edited, and a different tree: refused.
	World other;
	if (!build_world(other, 0, false, 70000)) { return; }
	Ref<GaussianSplatHlodRuntimeData> nothing;
	CHECK_FALSE(rebase_edited_snapshot(edited, other.resident, nothing)); // Another world object.
	CHECK_FALSE(rebase_edited_snapshot(edited, edited, nothing)); // The target must be unedited.
	Ref<GaussianSplatHlodRuntimeData> shifted;
	shifted.instantiate();
	shifted->source_world = resident->source_world;
	shifted->nodes = resident->nodes;
	shifted->high_order_count = resident->high_order_count;
	shifted->sh_degree = resident->sh_degree;
	shifted->leaf_count = resident->leaf_count;
	shifted->parent_count = resident->parent_count;
	shifted->nodes.write[leaf].topology.payload_first += 1; // Same world object, but the leaf's range moved: another tree.
	CHECK_FALSE(rebase_edited_snapshot(edited, shifted, nothing));
	CHECK(nothing.is_null());
}


// ---------------------------------------------------------------------------------------------------------------------
// Third fix round: route decision (F-1), instance-preserving blanking (F-2), publish failure (F-3), in-place release.
// ---------------------------------------------------------------------------------------------------------------------

// F-1: a committed edit forces streaming (a whole-tree bundle packs the unedited payload), and an edit that cannot be served
// builds NO route instead of a route that blanks for ever.
TEST_CASE("[GaussianSplatting][HLOD][Edit] a committed edit forces the streamed route, and an edit that cannot be served builds no route") {
	using Renderer = GaussianSplatRenderer;
	String reason;
	// No edit: the existing rule (stream only when the world is paged or does not fit).
	CHECK(Renderer::decide_hlod_route(false, false, false, false, nullptr, reason) == Renderer::HlodRouteDecision::BUILD_WHOLE_TREE);
	CHECK(Renderer::decide_hlod_route(false, false, false, true, nullptr, reason) == Renderer::HlodRouteDecision::BUILD_STREAMED);
	CHECK(Renderer::decide_hlod_route(false, false, true, false, nullptr, reason) == Renderer::HlodRouteDecision::BUILD_STREAMED);
	CHECK(Renderer::decide_hlod_route(false, false, false, true, "lod_disabled_canonical_leaves", reason) == Renderer::HlodRouteDecision::BUILD_WHOLE_TREE);
	// A committed edit on a world that FITS as a whole tree (the V4 cut toggle once the materialized world fits): streamed.
	CHECK(Renderer::decide_hlod_route(true, true, false, false, nullptr, reason) == Renderer::HlodRouteDecision::BUILD_STREAMED);
	CHECK(Renderer::decide_hlod_route(true, true, true, false, nullptr, reason) == Renderer::HlodRouteDecision::BUILD_STREAMED);
	// Streaming unsupported for this setup: refused, with the cause.
	CHECK(Renderer::decide_hlod_route(true, true, false, false, "shadow_instance_filter", reason) == Renderer::HlodRouteDecision::REFUSE);
	CHECK(reason == String("hlod_edit_requires_streamed_route:shadow_instance_filter"));
	// The committed edit could not be laid over this capture (rebase refused): refused, not served unedited.
	CHECK(Renderer::decide_hlod_route(true, false, false, true, nullptr, reason) == Renderer::HlodRouteDecision::REFUSE);
	CHECK(reason == String("hlod_edit_rebase_refused"));
	CHECK(Renderer::decide_hlod_route(true, false, true, true, nullptr, reason) == Renderer::HlodRouteDecision::REFUSE);
}

// F-2 / F-3: what the resident publication does with a frame of an edited world.
TEST_CASE("[GaussianSplatting][HLOD][Edit] a frame the edited route does not serve is skipped alone, or published without the edited world when other instances exist") {
	using Renderer = GaussianSplatRenderer;
	using Frame = Renderer::HlodEditFrame;
	// No committed edit: never touched, whatever the HLOD result was.
	CHECK(Renderer::decide_hlod_edit_frame(false, false, 0, true) == Frame::PUBLISH);
	CHECK(Renderer::decide_hlod_edit_frame(false, false, -1, false) == Frame::PUBLISH);
	// The edited route published and serves the edit: drawn.
	CHECK(Renderer::decide_hlod_edit_frame(true, false, 1, false) == Frame::PUBLISH);
	CHECK(Renderer::decide_hlod_edit_frame(true, false, 1, true) == Frame::PUBLISH);
	// The route does not serve the edit (released, rebuilt unedited): blanked.
	CHECK(Renderer::decide_hlod_edit_frame(true, true, 1, false) == Frame::SKIP_FRAME);
	CHECK(Renderer::decide_hlod_edit_frame(true, true, 0, false) == Frame::SKIP_FRAME);
	// F-3: the route still serves the edit but did NOT publish this frame (idle bundle failed, sort buffers invalid): the
	// canonical copy would be the unedited world.
	CHECK(Renderer::decide_hlod_edit_frame(true, false, 0, false) == Frame::SKIP_FRAME);
	CHECK(Renderer::decide_hlod_edit_frame(true, false, -1, false) == Frame::SKIP_FRAME);
	// F-2: another instance in the scene keeps rendering; only the edited world is omitted.
	CHECK(Renderer::decide_hlod_edit_frame(true, true, 0, true) == Frame::PUBLISH_WITHOUT_EDITED_WORLD);
	CHECK(Renderer::decide_hlod_edit_frame(true, false, -1, true) == Frame::PUBLISH_WITHOUT_EDITED_WORLD);
}

// The in-place release rule and the cause of a disabled cut.
TEST_CASE("[GaussianSplatting][HLOD][Edit] the committed edit applies to the same world only unless its content changed in place, and a disabled cut names itself") {
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	GS_NEED(renderer.is_valid());
	auto &state = renderer->get_resource_state();
	Ref<GaussianSplatHlodRuntimeData> edited, same_world_same_content, reimported, materialized;
	edited.instantiate();
	edited->edit_revision = 1;
	edited->source_revision = 7;
	same_world_same_content.instantiate();
	same_world_same_content->source_revision = 7;
	reimported.instantiate();
	reimported->source_revision = 8; // Same paged-ness (both resident), another content revision: replaced in place.
	materialized.instantiate();
	materialized->source_revision = 99; // The V4 world became resident: another paged-ness is a materialization, not a reimport.
	state.hlod_committed_edit = edited;
	state.hlod_owner_key = state.hlod_edit_owner_key = 5;
	state.hlod_world_key = state.hlod_edit_world_key = 6;
	CHECK(renderer->hlod_committed_edit_applies_to(same_world_same_content));
	CHECK_FALSE(renderer->hlod_committed_edit_applies_to(reimported));
	CHECK_FALSE(renderer->hlod_committed_edit_applies_to(Ref<GaussianSplatHlodRuntimeData>()));
	state.hlod_edit_world_key = 7; // Another world.
	CHECK_FALSE(renderer->hlod_committed_edit_applies_to(same_world_same_content));
	state.hlod_edit_world_key = 6;
	// A paged edit against a resident capture keeps applying (rebase), whatever the revision numbers say.
	Ref<GaussianSplatHlodRuntimeData> paged_edit;
	paged_edit.instantiate();
	paged_edit->edit_revision = 1;
	paged_edit->source_revision = 3;
	paged_edit->paged_source.instantiate();
	state.hlod_committed_edit = paged_edit;
	CHECK(renderer->hlod_committed_edit_applies_to(materialized));
	// The bare reason of a blanked frame names its cause: the disabled cut has no snapshot and so no reason of its own.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String key = "rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled";
	const bool had = ps->has_setting(key);
	const Variant previous = had ? ps->get_setting(key) : Variant();
	ps->set_setting(key, false);
	state.hlod_reason = "active"; // The last drawn frame's reason.
	String reason;
	CHECK(renderer->hlod_edit_blanks_frame(reason));
	CHECK(reason == String("hlod_edit_route_unavailable:hlod_runtime_cut_disabled"));
	if (had) { ps->set_setting(key, previous); } else { ps->clear(key); }
}


// Round 4: every blank frame states the CURRENT cause; the F-4 override clears the legacy bootstrap.
TEST_CASE("[GaussianSplatting][HLOD][Edit] a blanked frame names its current cause, never last frame's reason, and the override cancels the legacy bootstrap") {
	using Renderer = GaussianSplatRenderer;
	// The composer: an hlod_edit_* reason is kept, a generic or missing cause is replaced, a real cause is prefixed.
	CHECK(Renderer::compose_hlod_edit_blank_reason("hlod_edit_replacement_loading", true) == String("hlod_edit_replacement_loading"));
	CHECK(Renderer::compose_hlod_edit_blank_reason("hlod_idle_bundle_failed", true) == String("hlod_edit_route_unavailable:hlod_idle_bundle_failed"));
	CHECK(Renderer::compose_hlod_edit_blank_reason("active", true) == String("hlod_edit_route_unavailable")); // Last frame's "active" is no cause.
	// A tile-stage refusal of an earlier frame is a cause, kept by name and never overwritten; a blanked frame is not counted as one.
	CHECK(Renderer::compose_hlod_edit_blank_reason("hlod_tile_stage_refused:history_capacity", true) == String("hlod_edit_route_unavailable:hlod_tile_stage_refused:history_capacity"));
	GaussianRenderFacadeState::ResourceState blanked;
	blanked.hlod_frame_active = false; // The publisher clears it on a blanked frame.
	GaussianRenderFacadeState::hlod_mark_tile_stage_refused(blanked, "history_capacity");
	CHECK(blanked.hlod_tile_stage_refused_frames == 0);
	CHECK(blanked.hlod_reason.is_empty());
	CHECK(Renderer::compose_hlod_edit_blank_reason("active", false) == String("hlod_edit_route_unavailable:hlod_runtime_cut_disabled"));
	CHECK(Renderer::compose_hlod_edit_blank_reason("", false) == String("hlod_edit_route_unavailable:hlod_runtime_cut_disabled"));
	// A publish attempt starts from a generic cause: a return that sets no reason keeps neither last frame's "active" nor a stale cause.
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	GS_NEED(renderer.is_valid());
	auto &state = renderer->get_resource_state();
	Ref<GaussianSplatHlodRuntimeData> edited;
	edited.instantiate();
	edited->edit_revision = 1;
	state.hlod_committed_edit = edited;
	state.hlod_owner_key = state.hlod_edit_owner_key = 5;
	state.hlod_world_key = state.hlod_edit_world_key = 6;
	state.hlod_reason = "active";
	String ignored;
	CHECK(renderer->try_publish_hlod_resident_contract(ignored) == 0); // No snapshot: the route is not taken, no reason of its own.
	CHECK(state.hlod_reason == String("hlod_publish_failed"));
	state.hlod_reason = "hlod_edit_replacement_loading"; // A stale hlod_edit_* reason of an earlier frame.
	CHECK(renderer->try_publish_hlod_resident_contract(ignored) == 0);
	CHECK(state.hlod_reason == String("hlod_publish_failed"));
	state.hlod_committed_edit.unref(); // Without an edit the reason is not touched.
	state.hlod_reason = "active";
	CHECK(renderer->try_publish_hlod_resident_contract(ignored) == 0);
	CHECK(state.hlod_reason == String("active"));
	// F-4: the override also cancels the legacy streaming bootstrap (it runs before the bootstrap is attempted).
	Renderer::FrameBackendPlan plan;
	plan.prefer_resident_backend = false;
	plan.should_attempt_streaming_bootstrap = true;
	Renderer::apply_hlod_edit_backend_override(true, plan);
	CHECK(plan.prefer_resident_backend);
	CHECK_FALSE(plan.should_attempt_streaming_bootstrap);
	Renderer::FrameBackendPlan untouched;
	untouched.should_attempt_streaming_bootstrap = true;
	Renderer::apply_hlod_edit_backend_override(false, untouched);
	CHECK(untouched.should_attempt_streaming_bootstrap);
}

}
