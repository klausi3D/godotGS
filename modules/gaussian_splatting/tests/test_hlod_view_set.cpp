extern "C" int test_hlod_view_set_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../lod/hlod_view_set.h"
#include "../renderer/render_frame_context_manager.h"
#include <limits>

// ADR 14.1 / 14.2 / 6.9: stable node identity, per-view cuts and one shared residency budget. CPU only.
namespace TestHlodViewSet {
using namespace gs_hlod_runtime;

// Semantic binary tree: 0 root; 1,2 interiors (x = -2, +2); 3..6 leaves (x = -3,-1,1,3).
// `perm[slot]` is the semantic node stored at array slot `slot`; children stay contiguous, so a
// permuted array is a valid reordering (siblings swapped) of the same hierarchy.
static Ref<GaussianSplatHlodRuntimeData> make_tree(const uint32_t p_perm[7], uint64_t p_revision = 7, int64_t p_atlas_shift = 0) {
	Ref<GaussianSplatHlodRuntimeData> data;
	data.instantiate();
	data->source_revision = p_revision;
	data->nodes.resize(7);
	uint32_t slot_of[7];
	for (uint32_t s = 0; s < 7; ++s) { slot_of[p_perm[s]] = s; }
	const float x_of[7] = { 0, -2, 2, -3, -1, 1, 3 };
	for (uint32_t slot = 0; slot < 7; ++slot) {
		const uint32_t sem = p_perm[slot];
		auto &n = data->nodes.write[slot];
		n.bounds_center = Vector3(x_of[sem], 0, 0);
		n.topology.radius = sem == 0 ? 4.5f : (sem < 3 ? 2.5f : 0.5f);
		n.topology.payload_count = sem < 3 ? 2 : 4;
		n.topology.geometric_error = sem == 0 ? 4.0f : (sem < 3 ? 2.0f : 0.0f);
		n.topology.payload_first = uint64_t(1000 + 37 * slot + p_atlas_shift); // Atlas-like address: never part of identity.
		n.topology.cell.e = sem == 0 ? 3 : (sem < 3 ? 2 : 1);
		n.topology.cell.ix = int64_t(sem) * 5 - 2;
		n.topology.cell.iy = -int64_t(sem);
		n.topology.cell.iz = 1;
		if (sem < 3) {
			const uint32_t first_sem = sem * 2 + 1; // Semantic children of `sem`.
			const uint32_t a = slot_of[first_sem], b = slot_of[first_sem + 1];
			n.topology.first_child = MIN(a, b);
			n.topology.child_count = 2;
			CHECK(MAX(a, b) == MIN(a, b) + 1);
		}
		if (sem != 0) { n.topology.parent = slot_of[(sem - 1) / 2]; }
	}
	return data;
}
static gs_world_frame::PreciseCoord coord(double p_x, double p_y, double p_z) {
	const double v[3] = { p_x, p_y, p_z };
	gs_world_frame::PreciseCoord c;
	CHECK(gs_world_frame::PreciseCoord::from_doubles(v, c));
	return c;
}
static gs_world_frame::PrecisePlacement lifted(const Transform3D &p_transform) {
	gs_world_frame::PrecisePlacement p;
	CHECK(gs_world_frame::PrecisePlacement::from_frame_transform(p_transform, gs_world_frame::PreciseCoord(), p));
	return p;
}
static constexpr uint64_t ASSET = 0xA55E7;
static const uint32_t IDENT[7] = { 0, 1, 2, 3, 4, 5, 6 };
static const uint32_t SWAPPED[7] = { 0, 2, 1, 6, 5, 4, 3 };

static Caps caps(uint64_t p_splats = 100, uint32_t p_nodes = 8, double p_records = 1000) {
	Caps c;
	c.splat_budget = p_splats;
	c.node_budget = p_nodes;
	c.record_budget = p_records;
	return c;
}
static ViewSpec perspective(uint64_t p_key) {
	ViewSpec v;
	v.view_key = p_key;
	v.camera = Vector3(0, 0, 10);
	v.frustum_enabled = false;
	v.focal_pixels = 100;
	v.tau_pixels = 1;
	v.caps = caps();
	return v;
}
// Box frustum x in [-4, 0]: a parallel-plane view volume that sees only the left half. The camera is still a
// perspective point, so this is NOT an orthographic projection: the CPU oracle measures error by distance and
// error model; that model is part of the deferred renderer wiring.
static ViewSpec box_view(uint64_t p_key) {
	ViewSpec v = perspective(p_key);
	v.frustum_enabled = true;
	v.frustum[0] = Plane(Vector3(1, 0, 0), 0);
	v.frustum[1] = Plane(Vector3(-1, 0, 0), 4);
	v.frustum[2] = Plane(Vector3(0, 1, 0), 10);
	v.frustum[3] = Plane(Vector3(0, -1, 0), 10);
	v.frustum[4] = Plane(Vector3(0, 0, 1), 100);
	v.frustum[5] = Plane(Vector3(0, 0, -1), 100);
	return v;
}
static SelectionSnapshot snapshot(const Ref<GaussianSplatHlodRuntimeData> &p_data, uint64_t p_payload = 1000, uint64_t p_budget = 100000) {
	SelectionSnapshot s;
	s.hierarchy_revision = 1;
	SnapshotAsset a;
	a.asset_key = ASSET;
	a.data = p_data;
	a.bake_rule_revision = 1;
	a.payload_bytes = p_payload;
	s.assets.push_back(a);
	SnapshotInstance inst;
	inst.instance_key = 11;
	inst.asset_index = 0;
	inst.caps = caps();
	s.instances.push_back(inst);
	s.budget.physical_bytes = p_budget;
	s.budget.sort_bytes_per_splat = 4;
	s.budget.record_bytes_per_record = 1;
	s.budget.draw_bytes_per_node = 16;
	s.budget.max_views = 8;
	return s;
}
// GPU side: 100 splats * 4 + 1000 records * 1 + 8 nodes * 16; CPU side: per instance the cut workspace and tie
// ranks (7 nodes), plus published and staging draw records.
static uint64_t vb(uint32_t p_instances = 1) {
	const uint64_t history = uint64_t(p_instances) * ViewSet::slot_bytes(7);
	const uint64_t draw_capacity = std::min<uint64_t>(8, 7ull * p_instances);
	ResidencyBudget b;
	b.sort_bytes_per_splat = 4;
	b.record_bytes_per_record = 1;
	b.draw_bytes_per_node = 16;
	uint64_t bytes = 0;
	if (!ViewSet::view_cost(b, caps(), history, draw_capacity, bytes)) { return 0; }
	return bytes;
}
// Same charge written out from its parts, so the helper above cannot hide a missing term.
static uint64_t vb_independent(uint32_t p_instances = 1) {
	uint64_t ranks = 0, node_alloc = 0;
	allocation_bytes(7, sizeof(uint32_t), ranks);
	(void)node_alloc;
	const uint64_t draw_capacity = std::min<uint64_t>(8, 7ull * p_instances);
	return 400 + 1000 + 128 + 2 * draw_capacity * sizeof(DrawRecord) +
			p_instances * (CutHistory::workspace_bytes(7) + ranks);
}

static Vector<uint32_t> nodes_of(const ViewSet &p_set, uint64_t p_view) {
	Vector<uint32_t> out;
	uint32_t n = 0;
	const DrawRecord *r = p_set.drawn(p_view, n);
	for (uint32_t i = 0; r && i < n; ++i) { out.push_back(r[i].node); }
	return out;
}
// Copies the set's preallocated results into a caller-owned vector (test convenience; select() itself does not allocate).
static ViewSet::SnapshotStatus run(ViewSet &p_set, const SelectionSnapshot &p_snapshot, Vector<ViewSet::ViewResult> &r_out) {
	const ViewSet::SnapshotStatus status = p_set.select(p_snapshot);
	uint32_t n = 0;
	const ViewSet::ViewResult *src = p_set.results(n);
	r_out.clear();
	for (uint32_t i = 0; i < n; ++i) { r_out.push_back(src[i]); }
	return status;
}
static const ViewSet::ViewResult *result_of(const Vector<ViewSet::ViewResult> &p_results, uint64_t p_view) {
	for (int i = 0; i < p_results.size(); ++i) {
		if (p_results[i].view_key == p_view) { return &p_results[i]; }
	}
	return nullptr;
}
static bool same(const Vector<uint32_t> &a, std::initializer_list<uint32_t> b) {
	if (uint32_t(a.size()) != b.size()) { return false; }
	int i = 0;
	for (uint32_t v : b) { if (a[i++] != v) { return false; } }
	return true;
}

TEST_CASE("[GaussianSplatting][HLOD] NodeKey is stable under array reorder and atlas address change, NodeVersion tracks revisions") {
	auto a = make_tree(IDENT);
	auto b = make_tree(SWAPPED, 7, 123456);
	NodeKey ka[7], kb[7];
	for (uint32_t sem = 0; sem < 7; ++sem) {
		uint32_t slot_b = 0;
		for (uint32_t s = 0; s < 7; ++s) { if (SWAPPED[s] == sem) { slot_b = s; } }
		CHECK(make_node_key(*a.ptr(), sem, ASSET, ka[sem]));
		CHECK(make_node_key(*b.ptr(), slot_b, ASSET, kb[sem]));
		CHECK(ka[sem] == kb[sem]); // Same node, different array slot and different payload address.
		for (uint32_t other = 0; other < sem; ++other) { CHECK_FALSE(ka[sem] == ka[other]); }
	}
	NodeKey other_asset;
	CHECK(make_node_key(*a.ptr(), 3, ASSET + 1, other_asset));
	CHECK_FALSE(other_asset == ka[3]); // Asset identity is part of the key.
	NodeKey out_of_range;
	CHECK_FALSE(make_node_key(*a.ptr(), 7, ASSET, out_of_range));
	// The published draw sequence is keyed, so reordering the array does not reorder it.
	for (const Ref<GaussianSplatHlodRuntimeData> &tree : { a, b }) {
		ViewSet set;
		auto snap = snapshot(tree);
		CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
		snap.views.push_back(perspective(1));
		Vector<ViewSet::ViewResult> results;
		CHECK(run(set, snap, results) == ViewSet::SNAPSHOT_OK);
		uint32_t n = 0;
		const DrawRecord *r = set.drawn(1, n);
		CHECK(n == 4);
		for (uint32_t i = 0; r && i < n; ++i) {
			CHECK(r[i].key == ka[3 + i]); // Leaves in cell order, whatever slot they sit in.
			CHECK(r[i].version == (NodeVersion{ 7, 1 }));
		}
	}
	// Index-split group: siblings share a cell; the ordinal in the parent's range separates them and is
	// unchanged when the whole group is shifted in the array.
	for (const uint32_t first : { 1u, 5u }) {
		Ref<GaussianSplatHlodRuntimeData> g;
		g.instantiate();
		g->nodes.resize(first + 2);
		for (uint32_t i = 0; i < uint32_t(g->nodes.size()); ++i) {
			g->nodes.write[i].topology.cell.e = 4;
			g->nodes.write[i].topology.cell.ix = 2;
		}
		g->nodes.write[0].topology.first_child = first;
		g->nodes.write[0].topology.child_count = 2;
		for (uint32_t i = first; i < first + 2; ++i) {
			g->nodes.write[i].topology.flags = gs_hlod::kNodeFlagSplitByIndex;
			g->nodes.write[i].topology.parent = 0;
		}
		NodeKey k0, k1, root;
		CHECK(make_node_key(*g.ptr(), first, 1, k0));
		CHECK(make_node_key(*g.ptr(), first + 1, 1, k1));
		CHECK(make_node_key(*g.ptr(), 0, 1, root));
		CHECK_FALSE(k0 == k1);
		CHECK(k0.split_path == 0);
		CHECK(k1.split_path == 1);
		CHECK_FALSE(k0 == root);
	}
	// NodeVersion changes with the content revision and with the bake rule revision.
	auto c = make_tree(IDENT, 8);
	ViewSet set;
	auto snap = snapshot(c);
	snap.assets.write[0].bake_rule_revision = 2;
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	snap.views.push_back(perspective(1));
	Vector<ViewSet::ViewResult> results;
	run(set, snap, results);
	uint32_t n = 0;
	const DrawRecord *r = set.drawn(1, n);
	if (!r || n == 0) { FAIL("no draw records"); return; }
	CHECK(r[0].version == (NodeVersion{ 8, 2 }));
	CHECK_FALSE(r[0].version == (NodeVersion{ 7, 1 }));
}

TEST_CASE("[GaussianSplatting][HLOD] perspective and box-frustum views get independent complete cuts under one shared budget") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree, 1000, 1000 + 2 * vb(1)); // Exactly fits two views.
	ViewSet set;
	const auto p = set.admit_view(snap, perspective(1));
	const auto o = set.admit_view(snap, box_view(2));
	CHECK(p.status == ViewSet::ADMITTED);
	CHECK(o.status == ViewSet::ADMITTED);
	CHECK(p.view_bytes == vb(1));
	CHECK(o.charged_bytes == 1000 + vb(1)); // Payload once plus the first view.
	snap.views.push_back(perspective(1));
	snap.views.push_back(box_view(2));
	Vector<ViewSet::ViewResult> results;
	CHECK(run(set, snap, results) == ViewSet::SNAPSHOT_OK);
	CHECK(results.size() == 2);
	const auto *rp = result_of(results, 1);
	const auto *ro = result_of(results, 2);
	if (!rp || !ro) { return; }
	CHECK(rp->readiness == ViewSet::READY);
	CHECK(ro->readiness == ViewSet::READY);
	CHECK(same(nodes_of(set, 1), { 3, 4, 5, 6 }));
	CHECK(same(nodes_of(set, 2), { 3, 4 })); // Own frustum, own cut: the right half is outside this view.
	ViewSet::Ledger ledger;
	CHECK(set.ledger(snap, ledger));
	CHECK(ledger.shared_payload_bytes == 1000);
	CHECK(ledger.view_bytes == 2 * vb(1));
	CHECK(ledger.total() == snap.budget.physical_bytes); // At the boundary, not over it.
	// One byte less and the second view is refused, not squeezed in.
	auto tight = snapshot(tree, 1000, 1000 + 2 * vb(1) - 1);
	ViewSet tight_set;
	CHECK(tight_set.admit_view(tight, perspective(1)).status == ViewSet::ADMITTED);
	CHECK(tight_set.admit_view(tight, box_view(2)).status == ViewSet::REFUSED_BUDGET);
	CHECK(tight_set.view_count() == 1);
}

TEST_CASE("[GaussianSplatting][HLOD] a third view that does not fit is refused explicitly and leaves existing cuts unchanged") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree, 1000, 1000 + 2 * vb(1) + vb(1) / 2);
	ViewSet set;
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	CHECK(set.admit_view(snap, box_view(2)).status == ViewSet::ADMITTED);
	snap.views.push_back(perspective(1));
	snap.views.push_back(box_view(2));
	Vector<ViewSet::ViewResult> before;
	run(set, snap, before);
	const Vector<uint32_t> cut1 = nodes_of(set, 1), cut2 = nodes_of(set, 2);
	const auto refusal = set.admit_view(snap, perspective(3));
	CHECK(refusal.status == ViewSet::REFUSED_BUDGET);
	CHECK(refusal.charged_bytes + refusal.view_bytes > refusal.budget_bytes);
	CHECK(set.view_count() == 2);
	uint32_t n = 0;
	CHECK(set.drawn(3, n) == nullptr);
	snap.views.push_back(perspective(3)); // An unadmitted view in the snapshot is ignored, not partly served.
	Vector<ViewSet::ViewResult> after;
	CHECK(run(set, snap, after) == ViewSet::SNAPSHOT_OK);
	CHECK(after.size() == 2);
	CHECK(result_of(after, 3) == nullptr);
	CHECK(nodes_of(set, 1) == cut1);
	CHECK(nodes_of(set, 2) == cut2);
	for (const uint64_t key : { uint64_t(1), uint64_t(2) }) {
		CHECK(result_of(after, key)->readiness == ViewSet::READY);
		CHECK(result_of(after, key)->selected_splats == result_of(before, key)->selected_splats);
	}
	// Other explicit refusals.
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::REFUSED_DUPLICATE);
	auto limited = snap;
	limited.budget.max_views = 2;
	limited.budget.physical_bytes = 1000000;
	CHECK(set.admit_view(limited, perspective(4)).status == ViewSet::REFUSED_VIEW_LIMIT);
	ViewSpec big = perspective(5);
	big.caps = caps(10); // Instance caps (100 splats) no longer fit the view's caps.
	auto roomy = snap;
	roomy.budget.physical_bytes = 1000000;
	CHECK(set.admit_view(roomy, big).status == ViewSet::REFUSED_CAPS);
	// Removing a view releases its reservation so the refused one now fits.
	CHECK(set.remove_view(2));
	CHECK(set.admit_view(snap, perspective(3)).status == ViewSet::ADMITTED);
	CHECK(nodes_of(set, 1) == cut1);
}

TEST_CASE("[GaussianSplatting][HLOD] shared payload is charged once per asset, view allocations once per view") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree);
	SnapshotInstance second = snap.instances[0];
	second.instance_key = 12;
	second.placement.translation = coord(500, 0, 0);
	snap.instances.push_back(second); // Same asset, second placement.
	snap.instances.write[0].caps = caps(50, 4, 500);
	snap.instances.write[1].caps = caps(50, 4, 500);
	ViewSet set;
	ViewSet::Ledger ledger;
	CHECK(set.ledger(snap, ledger));
	CHECK(ledger.shared_payload_bytes == 1000); // Two instances, one payload.
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	CHECK(set.admit_view(snap, box_view(2)).status == ViewSet::ADMITTED);
	CHECK(set.ledger(snap, ledger));
	CHECK(ledger.shared_payload_bytes == 1000); // Two views do not recharge it.
	CHECK(ledger.view_bytes == 2 * vb(2)); // Views are charged from their own caps and instances, once each.
	// A distinct asset key is a distinct payload.
	SnapshotAsset other = snap.assets[0];
	other.asset_key = 0xB0B;
	snap.assets.push_back(other);
	snap.instances.write[1].asset_index = 1;
	CHECK(set.ledger(snap, ledger));
	CHECK(ledger.shared_payload_bytes == 2000);
	// Instance caps may not exceed the view caps they were reserved from.
	snap.instances.write[0].caps = caps(100, 4, 1000);
	ViewSet small;
	CHECK(small.admit_view(snap, perspective(1)).status == ViewSet::REFUSED_CAPS);
}

TEST_CASE("[GaussianSplatting][HLOD] instance transforms are applied to the view and non-uniform scale is refused") {
	auto tree = make_tree(IDENT);
	auto plain = snapshot(tree);
	ViewSet reference;
	CHECK(reference.admit_view(plain, box_view(1)).status == ViewSet::ADMITTED);
	plain.views.push_back(box_view(1));
	Vector<ViewSet::ViewResult> r0;
	run(reference, plain, r0);
	const Vector<uint32_t> expected = nodes_of(reference, 1);
	CHECK(same(expected, { 3, 4 }));
	// Place the instance (rotation + uniform scale 2 + translation) and move the same view with it.
	const Transform3D placement(Basis(Vector3(0, 1, 0), 0.7).scaled(Vector3(2, 2, 2)), Vector3(40, -3, 25));
	ViewSpec local = box_view(1);
	ViewSpec world = local;
	world.camera = placement.xform(local.camera);
	for (int i = 0; i < 6; ++i) {
		const Vector3 point = local.frustum[i].normal * local.frustum[i].d;
		const Vector3 wp = placement.xform(point);
		const Vector3 wn = placement.basis.xform(local.frustum[i].normal).normalized();
		world.frustum[i] = Plane(wn, wn.dot(wp));
	}
	auto placed = snapshot(tree);
	placed.instances.write[0].placement = lifted(placement);
	ViewSet set;
	CHECK(set.admit_view(placed, world).status == ViewSet::ADMITTED);
	placed.views.push_back(world);
	Vector<ViewSet::ViewResult> r1;
	run(set, placed, r1);
	CHECK(same(nodes_of(set, 1), { 3, 4 })); // Scale and distance cancel: same cut as in the asset frame.
	// The unmoved world view no longer sees the placed instance the same way: transform is not ignored.
	ViewSet naive;
	auto moved_only = snapshot(tree);
	moved_only.instances.write[0].placement = lifted(placement);
	CHECK(naive.admit_view(moved_only, local).status == ViewSet::ADMITTED);
	moved_only.views.push_back(local);
	Vector<ViewSet::ViewResult> r2;
	run(naive, moved_only, r2);
	CHECK_FALSE(same(nodes_of(naive, 1), { 3, 4 }));
	// Non-uniform scale cannot keep error and distance consistent: refused up front.
	auto skew = snapshot(tree);
	skew.instances.write[0].placement.basis = Basis().scaled(Vector3(1, 2, 1));
	ViewSet refused;
	CHECK(refused.admit_view(skew, perspective(1)).status == ViewSet::REFUSED_INVALID);
}

TEST_CASE("[GaussianSplatting][HLOD] per-view publication is deterministic, order independent and reaches a static fixed point") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree, 1000, 100000);
	SnapshotInstance second = snap.instances[0];
	second.instance_key = 5; // Sorts before instance 11 whatever the listing order.
	second.placement.translation = coord(0.5, 0, 0);
	snap.instances.push_back(second);
	snap.instances.write[0].caps = caps(50, 4, 500);
	snap.instances.write[1].caps = caps(50, 4, 500);
	auto reversed = snap;
	reversed.instances.write[0] = snap.instances[1];
	reversed.instances.write[1] = snap.instances[0];
	for (auto *s : { &snap, &reversed }) {
		s->views.push_back(box_view(2));
		s->views.push_back(perspective(1));
	}
	ViewSet forward, backward;
	CHECK(forward.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	CHECK(forward.admit_view(snap, box_view(2)).status == ViewSet::ADMITTED);
	CHECK(backward.admit_view(reversed, box_view(2)).status == ViewSet::ADMITTED); // Opposite admission order.
	CHECK(backward.admit_view(reversed, perspective(1)).status == ViewSet::ADMITTED);
	Vector<ViewSet::ViewResult> rf, rb;
	uint64_t previous_records[2] = { 0, 0 };
	Vector<DrawRecord> first_frame[2];
	for (int frame = 0; frame < 4; ++frame) {
		CHECK(run(forward, snap, rf) == ViewSet::SNAPSHOT_OK);
		CHECK(run(backward, reversed, rb) == ViewSet::SNAPSHOT_OK);
		if (rf.size() != 2 || rb.size() != 2) { FAIL("expected two results"); return; }
		CHECK(rf[0].view_key == 1); // Ascending view key regardless of snapshot or admission order.
		CHECK(rf[1].view_key == 2);
		for (int v = 0; v < 2; ++v) {
			const uint64_t key = v + 1;
			uint32_t nf = 0, nb = 0;
			const DrawRecord *df = forward.drawn(key, nf);
			const DrawRecord *db = backward.drawn(key, nb);
			CHECK(nf == nb);
			CHECK(nf > 0);
			for (uint32_t i = 0; i < nf && i < nb; ++i) {
				CHECK(df[i].instance_key == db[i].instance_key);
				CHECK(df[i].key == db[i].key);
				CHECK(df[i].node == db[i].node);
				if (i > 0) { CHECK(df[i - 1].instance_key <= df[i].instance_key); }
			}
			CHECK(rf[v].readiness == ViewSet::READY);
			if (frame == 0) {
				first_frame[v].resize(nf);
				for (uint32_t i = 0; i < nf; ++i) { first_frame[v].write[i] = df[i]; }
			} else {
				CHECK(nf == uint32_t(first_frame[v].size())); // Static camera: fixed point from the first frame.
				for (uint32_t i = 0; i < nf && i < uint32_t(first_frame[v].size()); ++i) {
					CHECK(df[i].key == first_frame[v][i].key);
					CHECK(df[i].instance_key == first_frame[v][i].instance_key);
				}
			}
			previous_records[v] = rf[v].selected_splats;
		}
	}
	CHECK(previous_records[0] > previous_records[1]); // The two views really differ.
}

TEST_CASE("[GaussianSplatting][HLOD] readiness is loading, ready, not admitted or failed and only a complete frontier is ready") {
	auto tree = make_tree(IDENT);
	// Heavy root: root payload 100 exceeds the splat cap, so the complete child frontier is required.
	tree->nodes.write[0].topology.payload_count = 100;
	tree->nodes.write[0].topology.radius = 50.0f;
	for (int i = 1; i < 3; ++i) { tree->nodes.write[i].topology.payload_count = 40; }
	auto snap = snapshot(tree);
	snap.instances.write[0].caps = caps(90, 7, 1000);
	ViewSpec inside = perspective(1);
	inside.camera = Vector3(0, 0, 0);
	inside.tau_pixels = 1e9; // No refinement wanted: isolate the bootstrap frontier.
	inside.caps = caps(90, 7, 1000);
	ViewSet set;
	CHECK(set.admit_view(snap, inside).status == ViewSet::ADMITTED);
	ViewSpec other = box_view(2);
	other.caps = caps(90, 7, 1000);
	other.tau_pixels = 1e9;
	snap.assets.write[0].resident = Vector<uint8_t>();
	CHECK(set.admit_view(snap, other).status == ViewSet::ADMITTED);
	// Loading: interior 2 is not resident, so the frontier {1, 2} cannot be completed.
	Vector<uint8_t> resident;
	resident.resize(7);
	resident.fill(1);
	resident.write[2] = 0;
	snap.assets.write[0].resident = resident;
	snap.views.push_back(inside);
	snap.views.push_back(other);
	Vector<ViewSet::ViewResult> results;
	run(set, snap, results);
	const auto *a = result_of(results, 1);
	const auto *b = result_of(results, 2);
	if (!a || !b) { return; }
	CHECK(a->readiness == ViewSet::LOADING);
	CHECK(a->worst_status == CutHistory::BOOTSTRAP_MISSING_RESIDENCY);
	CHECK(a->drawn_count == 0);
	CHECK(a->draw_generation == 0); // Nothing was published as ready.
	uint32_t n = 0;
	set.drawn(1, n);
	CHECK(n == 0);
	CHECK(b->readiness == ViewSet::LOADING); // Its frontier needs interior 1 only, but the root is over the cap too.
	// Full residency: the whole frontier is present, so the view becomes ready with exactly that frontier.
	snap.assets.write[0].resident = Vector<uint8_t>();
	run(set, snap, results);
	a = result_of(results, 1);
	CHECK(a->readiness == ViewSet::READY);
	CHECK(same(nodes_of(set, 1), { 1, 2 }));
	CHECK(a->draw_generation == 1);
	// Not admitted: no complete frontier fits the splat cap (leaves hold 16). The other view still fits.
	const Vector<uint32_t> before = nodes_of(set, 1);
	snap.instances.write[0].caps = caps(10, 7, 1000);
	run(set, snap, results);
	CHECK(result_of(results, 1)->readiness == ViewSet::NOT_ADMITTED);
	CHECK(result_of(results, 1)->worst_status == CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(before.size() == 2);
	CHECK(nodes_of(set, 1).is_empty()); // A stale set is never exposed once the view is not ready.
	CHECK(result_of(results, 1)->drawn_count == 0);
	CHECK(result_of(results, 2)->readiness == ViewSet::READY); // Its visible frontier (2 leaves, 8 splats) fits.
	snap.instances.write[0].caps = caps(90, 7, 1000);
	// Failed: invalid arithmetic input.
	snap.views.write[0].tau_pixels = std::numeric_limits<double>::quiet_NaN();
	run(set, snap, results);
	CHECK(result_of(results, 1)->readiness == ViewSet::FAILED);
	// Exceeding the reservation is explicit and selects nothing.
	snap.views.write[0].tau_pixels = 1e9;
	snap.views.write[0].caps = caps(200, 7, 1000);
	run(set, snap, results);
	CHECK(result_of(results, 1)->readiness == ViewSet::NOT_ADMITTED);
	CHECK(result_of(results, 1)->reservation_exceeded);
}

TEST_CASE("[GaussianSplatting][HLOD] a payload that outgrows the shared budget is reported without selecting or touching admitted views") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree, 1000, 1000 + vb(1));
	ViewSet set;
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	snap.views.push_back(perspective(1));
	Vector<ViewSet::ViewResult> results;
	CHECK(run(set, snap, results) == ViewSet::SNAPSHOT_OK);
	const Vector<uint32_t> cut = nodes_of(set, 1);
	if (results.size() != 1) { FAIL("expected one result, got ", results.size()); return; }
	const uint64_t generation = results[0].draw_generation;
	snap.assets.write[0].payload_bytes = 1001; // The shared payload grew past the budget.
	CHECK(run(set, snap, results) == ViewSet::SNAPSHOT_OVER_BUDGET);
	if (results.size() != 1) { FAIL("expected one result, got ", results.size()); return; }
	CHECK(results[0].readiness == ViewSet::NOT_ADMITTED);
	CHECK(results[0].reservation_exceeded);
	CHECK(cut.size() == 4);
	CHECK(nodes_of(set, 1).is_empty()); // Not ready: the earlier set is no longer exposed.
	snap.assets.write[0].payload_bytes = 1000;
	CHECK(run(set, snap, results) == ViewSet::SNAPSHOT_OK);
	if (results.size() != 1) { FAIL("expected one result, got ", results.size()); return; }
	CHECK(results[0].draw_generation == generation + 1);
	snap.instances.write[0].asset_index = 5; // Malformed snapshot.
	CHECK(run(set, snap, results) == ViewSet::SNAPSHOT_INVALID);
}

static Vector<NodeKey> keys_of(const ViewSet &p_set, uint64_t p_view) {
	Vector<NodeKey> out;
	uint32_t n = 0;
	const DrawRecord *r = p_set.drawn(p_view, n);
	for (uint32_t i = 0; r && i < n; ++i) { out.push_back(r[i].key); }
	return out;
}

TEST_CASE("[GaussianSplatting][HLOD] view set charges real allocated bytes: cut workspaces and draw storage count") {
	CHECK(vb(1) == vb_independent(1));
	CHECK(vb(2) == vb_independent(2));
	auto tree = make_tree(IDENT);
	const uint64_t storage = 2 * 7 * sizeof(DrawRecord);
	const uint64_t history = ViewSet::slot_bytes(7);
	CHECK(history > CutHistory::workspace_bytes(7)); // Tie ranks are part of the slot.
	// The GPU-side figure (all the first version charged) plus draw storage still leaves the history out: refused.
	ViewSet short_set;
	auto without_history = snapshot(tree, 1000, 1000 + 1528 + storage);
	const auto refused = short_set.admit_view(without_history, perspective(1));
	CHECK(refused.status == ViewSet::REFUSED_BUDGET);
	CHECK(short_set.view_count() == 0);
	CHECK(refused.view_bytes == 1528 + storage + history);
	// One byte under the full charge is refused, the exact charge is admitted.
	ViewSet under;
	CHECK(under.admit_view(snapshot(tree, 1000, 1000 + vb(1) - 1), perspective(1)).status == ViewSet::REFUSED_BUDGET);
	ViewSet exact;
	CHECK(exact.admit_view(snapshot(tree, 1000, 1000 + vb(1)), perspective(1)).status == ViewSet::ADMITTED);
	// A second instance adds its own workspace to the same view.
	auto two = snapshot(tree, 1000, 1000 + vb(2) - 1);
	SnapshotInstance second = two.instances[0];
	second.instance_key = 12;
	two.instances.write[0].caps = caps(50, 4, 500);
	second.caps = caps(50, 4, 500);
	two.instances.push_back(second);
	ViewSet two_set;
	CHECK(two_set.admit_view(two, perspective(1)).status == ViewSet::REFUSED_BUDGET);
	two.budget.physical_bytes = 1000 + vb(2);
	CHECK(two_set.admit_view(two, perspective(1)).status == ViewSet::ADMITTED);
}

TEST_CASE("[GaussianSplatting][HLOD] admission refuses overflow, NaN and out-of-range caps explicitly") {
	auto tree = make_tree(IDENT);
	const double nan = std::numeric_limits<double>::quiet_NaN();
	// mul overflow in the view charge: splats * bytes-per-splat wraps.
	{
		auto snap = snapshot(tree);
		snap.instances.write[0].caps = caps(UINT64_MAX / 2, 8, 1000);
		ViewSpec v = perspective(1);
		v.caps = caps(UINT64_MAX / 2, 8, 1000);
		ViewSet set;
		CHECK(set.admit_view(snap, v).status == ViewSet::REFUSED_BUDGET);
		CHECK(set.view_count() == 0);
		uint64_t out = 0;
		ResidencyBudget b = snap.budget;
		CHECK_FALSE(ViewSet::view_cost(b, v.caps, 0, 7, out));
		b.record_bytes_per_record = UINT64_MAX;
		CHECK_FALSE(ViewSet::view_cost(b, caps(), 0, 7, out)); // records * bytes overflows
		b = snap.budget;
		CHECK_FALSE(ViewSet::view_cost(b, caps(), UINT64_MAX, 7, out)); // add overflow
		CHECK(ViewSet::view_cost(b, caps(), 0, 7, out));
	}
	// add overflow in the shared ledger: two distinct payloads that do not fit in 64 bits together.
	{
		auto snap = snapshot(tree);
		snap.assets.write[0].payload_bytes = UINT64_MAX;
		SnapshotAsset other = snap.assets[0];
		other.asset_key = 0xB0B;
		other.payload_bytes = 1;
		snap.assets.push_back(other);
		SnapshotInstance second = snap.instances[0];
		second.instance_key = 12;
		second.asset_index = 1;
		snap.instances.write[0].caps = caps(50, 4, 500);
		second.caps = caps(50, 4, 500);
		snap.instances.push_back(second);
		ViewSet set;
		ViewSet::Ledger ledger;
		CHECK_FALSE(set.ledger(snap, ledger));
		CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::REFUSED_BUDGET);
		CHECK(set.view_count() == 0);
	}
	// Instance caps whose sum overflows (splats, then nodes) never fit the view.
	{
		auto snap = snapshot(tree);
		SnapshotInstance second = snap.instances[0];
		second.instance_key = 12;
		snap.instances.write[0].caps = caps(UINT64_MAX / 2 + 1, 4, 10);
		second.caps = caps(UINT64_MAX / 2 + 1, 4, 10);
		snap.instances.push_back(second);
		ViewSpec v = perspective(1);
		v.caps = caps(UINT64_MAX, 8, 1000);
		ViewSet set;
		CHECK(set.admit_view(snap, v).status == ViewSet::REFUSED_CAPS);
		snap.instances.write[0].caps = caps(10, UINT32_MAX, 10);
		snap.instances.write[1].caps = caps(10, UINT32_MAX, 10);
		v.caps = caps(100, UINT32_MAX, 1000);
		CHECK(set.admit_view(snap, v).status == ViewSet::REFUSED_CAPS);
		CHECK(set.view_count() == 0);
	}
	// NaN, infinite and absurd caps are invalid, on the view and on an instance.
	{
		ViewSet set;
		ViewSpec v = perspective(1);
		v.caps.record_budget = nan;
		CHECK(set.admit_view(snapshot(tree), v).status == ViewSet::REFUSED_INVALID);
		v.caps.record_budget = std::numeric_limits<double>::infinity();
		CHECK(set.admit_view(snapshot(tree), v).status == ViewSet::REFUSED_INVALID);
		v.caps.record_budget = 1.0e19;
		CHECK(set.admit_view(snapshot(tree), v).status == ViewSet::REFUSED_INVALID);
		v = perspective(1);
		v.caps.splat_budget = 0;
		CHECK(set.admit_view(snapshot(tree), v).status == ViewSet::REFUSED_INVALID);
		auto bad = snapshot(tree);
		bad.instances.write[0].caps.record_budget = nan;
		CHECK(set.admit_view(bad, perspective(1)).status == ViewSet::REFUSED_INVALID);
		CHECK(set.view_count() == 0);
	}
}

TEST_CASE("[GaussianSplatting][HLOD] a view with one loading instance is loading and publishes nothing") {
	auto tree_a = make_tree(IDENT);
	auto tree_b = make_tree(IDENT, 9);
	auto snap = snapshot(tree_a);
	SnapshotAsset b;
	b.asset_key = 0xB0B;
	b.data = tree_b;
	b.bake_rule_revision = 1;
	b.payload_bytes = 500;
	snap.assets.push_back(b);
	SnapshotInstance inst_b = snap.instances[0];
	inst_b.instance_key = 22;
	inst_b.asset_index = 1;
	snap.instances.write[0].caps = caps(50, 4, 500);
	inst_b.caps = caps(50, 4, 500);
	snap.instances.push_back(inst_b);
	ViewSet set;
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	snap.views.push_back(perspective(1));
	// Instance A (key 11) is fully resident; instance B (key 22) has no resident root yet.
	Vector<uint8_t> none;
	none.resize(7);
	none.fill(1);
	none.write[0] = 0;
	snap.assets.write[1].resident = none;
	Vector<ViewSet::ViewResult> results;
	CHECK(run(set, snap, results) == ViewSet::SNAPSHOT_OK);
	if (results.size() != 1) { FAIL("expected one result"); return; }
	CHECK(results[0].readiness == ViewSet::LOADING);
	CHECK(results[0].worst_status == CutHistory::UNAVAILABLE);
	CHECK(results[0].drawn_count == 0);
	CHECK(results[0].draw_generation == 0);
	uint32_t n = 99;
	CHECK(set.drawn(1, n) == nullptr);
	CHECK(n == 0); // A's complete cut is not exposed on its own.
	snap.assets.write[1].resident = Vector<uint8_t>();
	CHECK(run(set, snap, results) == ViewSet::SNAPSHOT_OK);
	if (results.size() != 1) { FAIL("expected one result"); return; }
	CHECK(results[0].readiness == ViewSet::READY);
	const DrawRecord *r = set.drawn(1, n);
	if (!r || n != 8) { FAIL("expected both instances' leaves"); return; }
	CHECK(r[0].instance_key == 11);
	CHECK(r[n - 1].instance_key == 22);
}

TEST_CASE("[GaussianSplatting][HLOD] a drawn node that loses residency never stays in the published set") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree);
	ViewSet set;
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	snap.views.push_back(perspective(1));
	Vector<ViewSet::ViewResult> results;
	run(set, snap, results);
	CHECK(same(nodes_of(set, 1), { 3, 4, 5, 6 }));
	const uint64_t generation = results.size() == 1 ? results[0].draw_generation : 0;
	// Steady state: leaf 5 is evicted. The new cut keeps its resident parent instead of the evicted leaf.
	Vector<uint8_t> resident;
	resident.resize(7);
	resident.fill(1);
	resident.write[5] = 0;
	snap.assets.write[0].resident = resident;
	run(set, snap, results);
	if (results.size() != 1) { FAIL("expected one result"); return; }
	CHECK(results[0].readiness == ViewSet::READY);
	CHECK(results[0].draw_generation == generation + 1);
	const Vector<uint32_t> after = nodes_of(set, 1);
	CHECK(same(after, { 3, 4, 2 }));
	for (int i = 0; i < after.size(); ++i) { CHECK(resident[after[i]] == 1); }
	// The root itself is evicted: nothing complete exists, so the view is loading and exposes nothing.
	resident.write[0] = 0;
	snap.assets.write[0].resident = resident;
	run(set, snap, results);
	if (results.size() != 1) { FAIL("expected one result"); return; }
	CHECK(results[0].readiness == ViewSet::LOADING);
	CHECK(nodes_of(set, 1).is_empty());
	// Residency returns: ready again with the full cut.
	snap.assets.write[0].resident = Vector<uint8_t>();
	run(set, snap, results);
	if (results.size() != 1) { FAIL("expected one result"); return; }
	CHECK(results[0].readiness == ViewSet::READY);
	CHECK(same(nodes_of(set, 1), { 3, 4, 5, 6 }));
}

TEST_CASE("[GaussianSplatting][HLOD] equal-priority ties break on the stable key, not the array index, under budget pressure") {
	auto a = make_tree(IDENT);
	auto b = make_tree(SWAPPED); // Same hierarchy, sibling order reversed in the array.
	Vector<NodeKey> cuts[2];
	int idx = 0;
	for (const Ref<GaussianSplatHlodRuntimeData> &tree : { a, b }) {
		auto snap = snapshot(tree);
		snap.instances.write[0].caps = caps(10, 8, 1000); // Refining both interiors (16 splats) is refused.
		ViewSpec v = perspective(1);
		v.caps = caps(10, 8, 1000);
		ViewSet set;
		CHECK(set.admit_view(snap, v).status == ViewSet::ADMITTED);
		snap.views.push_back(v);
		Vector<ViewSet::ViewResult> results;
		run(set, snap, results);
		if (results.size() != 1) { FAIL("expected one result"); return; }
		CHECK(results[0].readiness == ViewSet::READY);
		CHECK(results[0].budget_bound); // The refusal really happened.
		CHECK(results[0].selected_splats == 10);
		cuts[idx++] = keys_of(set, 1);
	}
	if (cuts[0].size() != 3 || cuts[1].size() != 3) { FAIL("expected three-node cuts"); return; }
	for (int i = 0; i < 3; ++i) { CHECK(cuts[0][i] == cuts[1][i]); }
	// And the winner is the smaller key (semantic node 1's subtree), in both array orders.
	NodeKey leaf3;
	CHECK(make_node_key(*a.ptr(), 3, ASSET, leaf3));
	CHECK(cuts[0][0] == leaf3);
	// CutHistory with no rank keeps the pre-existing index tie-break (B1 behaviour is unchanged).
	CutHistory history;
	CHECK(history.reserve(7));
	CutRequest r;
	r.frustum_enabled = false;
	r.camera = Vector3(0, 0, 10);
	r.focal_pixels = 100;
	r.tau_pixels = 1;
	r.record_budget = 1000;
	r.splat_budget = 10;
	r.node_budget = 7;
	CHECK(history.select(*b.ptr(), r).selected_count == 3);
	// Slot 1 holds semantic node 2 in the swapped array: index order refines it (its leaves, x = +1 and +3, sit in
	// slots 3 and 4), the opposite of the key order used above.
	CHECK(b->nodes[history.selected_node(1)].bounds_center.x == 3.0f);
}

TEST_CASE("[GaussianSplatting][HLOD] a changed tau invalidates the refinement history") {
	auto tree = make_tree(IDENT);
	ViewSpec v = perspective(1);
	auto snap = snapshot(tree);
	ViewSet set;
	CHECK(set.admit_view(snap, v).status == ViewSet::ADMITTED);
	Vector<ViewSet::ViewResult> results;
	snap.views.push_back(v);
	run(set, snap, results); // tau 1: everything refined, interiors stamped as refined.
	CHECK(same(nodes_of(set, 1), { 3, 4, 5, 6 }));
	// tau 22: interior error ~26 lies in the hysteresis band (22, 27.5]. With stale stamps they would stay
	// refined; with the stamps dropped for the new policy the cut is what a cold start gives.
	snap.views.write[0].tau_pixels = 22;
	run(set, snap, results);
	ViewSet cold;
	CHECK(cold.admit_view(snap, v).status == ViewSet::ADMITTED);
	Vector<ViewSet::ViewResult> cold_results;
	run(cold, snap, cold_results);
	CHECK(same(nodes_of(cold, 1), { 1, 2 }));
	CHECK(same(nodes_of(set, 1), { 1, 2 }));
	// Unchanged policy keeps its history: next frame is a fixed point.
	run(set, snap, results);
	CHECK(same(nodes_of(set, 1), { 1, 2 }));
}

TEST_CASE("[GaussianSplatting][HLOD] a snapshot that outgrows a view's reservation is reported, not failed, until re-admitted") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree, 1000, 1000000);
	snap.instances.write[0].caps = caps(50, 4, 500);
	ViewSet set;
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED); // View caps (100, 8, 1000) leave room.
	snap.views.push_back(perspective(1));
	Vector<ViewSet::ViewResult> results;
	run(set, snap, results);
	if (results.size() != 1) { FAIL("expected one result"); return; }
	CHECK(results[0].readiness == ViewSet::READY);
	SnapshotInstance second = snap.instances[0];
	second.instance_key = 12;
	snap.instances.push_back(second); // Within the view's caps, but its workspace and draw storage were never reserved.
	CHECK(run(set, snap, results) == ViewSet::SNAPSHOT_OK);
	if (results.size() != 1) { FAIL("expected one result"); return; }
	CHECK(results[0].readiness == ViewSet::NOT_ADMITTED);
	CHECK(results[0].reservation_exceeded);
	CHECK(nodes_of(set, 1).is_empty());
	CHECK(set.remove_view(1));
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	run(set, snap, results);
	if (results.size() != 1) { FAIL("expected one result"); return; }
	CHECK(results[0].readiness == ViewSet::READY);
}

TEST_CASE("[GaussianSplatting][HLOD] steady-state selection allocates nothing") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree);
	SnapshotInstance second = snap.instances[0];
	second.instance_key = 5;
	snap.instances.write[0].caps = caps(50, 4, 500);
	second.caps = caps(50, 4, 500);
	snap.instances.push_back(second);
	ViewSet set;
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	CHECK(set.admit_view(snap, box_view(2)).status == ViewSet::ADMITTED);
	snap.views.push_back(perspective(1));
	snap.views.push_back(box_view(2));
	// Control: the counter below does see an ordinary Vector allocation.
	const uint64_t control_before = Memory::get_mem_usage();
	Vector<uint8_t> control;
	control.resize(4096);
	CHECK(Memory::get_mem_usage() > control_before);
	for (int i = 0; i < 3; ++i) { set.select(snap); } // Warm-up.
	const uint64_t bytes = set.allocated_bytes();
	const uint64_t usage = Memory::get_mem_usage();
	uint32_t count_before = 0;
	const ViewSet::ViewResult *results_before = set.results(count_before);
	for (int frame = 0; frame < 64; ++frame) {
		// Move the camera and change tau: the work changes, the storage must not.
		snap.views.write[0].camera = Vector3(0.01f * frame, 0, 10 + 0.02f * frame);
		snap.views.write[1].tau_pixels = (frame & 1) ? 1.0 : 1.5;
		CHECK(set.select(snap) == ViewSet::SNAPSHOT_OK);
	}
	uint32_t count_after = 0;
	CHECK(set.results(count_after) == results_before);
	CHECK(count_after == 2);
	CHECK(set.allocated_bytes() == bytes);
	CHECK(Memory::get_mem_usage() == usage);
	// Admitting more work is allowed to allocate; that is the contrast the counter must show.
	const uint64_t before_admit = Memory::get_mem_usage();
	CHECK(set.admit_view(snap, perspective(3)).status == ViewSet::ADMITTED);
	CHECK(Memory::get_mem_usage() > before_admit);
}

// ---- Package B3: the renderer per-view cuts live in ViewSet, owned through RenderFrameContextManager. ----
// CPU only: select_hlod_view() is the same entry the render path calls; the RenderSceneBuffersRD token lookup
// (acquire_hlod_identity) is the only part that needs a live buffers object and is covered by the GPU gate.
static bool all_live(const HlodViewIdentity &) { return true; } // Fake buffers ids: nothing resolves in ObjectDB.
static HlodViewIdentity identity_of(uint64_t p_buffers, uint64_t p_target, uint64_t p_epoch) {
	HlodViewIdentity id;
	id.buffers = ObjectID(p_buffers);
	id.target = RID::from_uint64(p_target);
	id.epoch = p_epoch;
	return id;
}

TEST_CASE("[GaussianSplatting][HLOD] renderer view is admitted, reused, refused and re-admitted across buffer lifetime and epoch") {
	auto tree = make_tree(IDENT);
	SelectionSnapshot snap = snapshot(tree, 0, 1000000);
	RenderFrameContextManager mgr;
	mgr.set_hlod_liveness_check(&all_live);
	ViewSpec spec = perspective(0);
	const HlodViewIdentity a = identity_of(101, 7, 5);

	HlodViewOutcome out = mgr.select_hlod_view(a, 1, snap, spec);
	CHECK(out.active());
	CHECK(out.admitted_now);
	CHECK(out.view.draw_generation == 1);
	CHECK(out.record_count == 4);
	CHECK(mgr.hlod_bound_view_count() == 1);
	CHECK(mgr.get_hlod_view_set().view_count() == 1);

	// Same buffers, target, epoch and snapshot: the same view, its history and generation carry on.
	out = mgr.select_hlod_view(a, 1, snap, spec);
	CHECK(out.active());
	CHECK_FALSE(out.admitted_now);
	CHECK(out.view.draw_generation == 2);

	// The buffers object was reconfigured (new lifetime epoch): the old view is dropped and a new one admitted.
	HlodViewIdentity next_epoch = a;
	next_epoch.epoch = 6;
	out = mgr.select_hlod_view(next_epoch, 1, snap, spec);
	CHECK(out.active());
	CHECK(out.admitted_now);
	CHECK(out.view.draw_generation == 1);
	CHECK(mgr.hlod_bound_view_count() == 1);
	CHECK(mgr.get_hlod_view_set().view_count() == 1); // Nothing leaked from the old epoch.
	CHECK(RenderFrameContextManager::hlod_view_key(a) != RenderFrameContextManager::hlod_view_key(next_epoch));

	// A different render target or snapshot object also starts a new view (the old slot rules).
	HlodViewIdentity next_target = next_epoch;
	next_target.target = RID::from_uint64(8);
	out = mgr.select_hlod_view(next_target, 1, snap, spec);
	CHECK(out.admitted_now);
	out = mgr.select_hlod_view(next_target, 2, snap, spec);
	CHECK(out.admitted_now);
	CHECK(mgr.get_hlod_view_set().view_count() == 1);

	// Refusal: a budget that cannot hold another view is explicit, leaves no half-admitted state and does
	// not disturb the admitted view.
	const HlodViewIdentity b = identity_of(202, 9, 5);
	SelectionSnapshot tiny = snapshot(tree, 0, 10);
	out = mgr.select_hlod_view(b, 1, tiny, spec);
	CHECK_FALSE(out.active());
	CHECK(String(out.fallback.reason) == "hlod_view_budget_refused");
	CHECK(out.record_count == 0);
	CHECK(out.records == nullptr);
	CHECK(mgr.hlod_bound_view_count() == 1);
	CHECK(mgr.get_hlod_view_set().view_count() == 1);
	// The refused buffers is re-admitted as soon as the budget allows it.
	out = mgr.select_hlod_view(b, 1, snap, spec);
	CHECK(out.active());
	CHECK(out.admitted_now);
	CHECK(mgr.get_hlod_view_set().view_count() == 2);
	out = mgr.select_hlod_view(next_target, 2, snap, spec);
	CHECK_FALSE(out.admitted_now);

	// Four live views at most: the fifth evicts the least recently used one, which is re-admitted on return.
	CHECK(mgr.select_hlod_view(identity_of(303, 10, 5), 1, snap, spec).active());
	CHECK(mgr.select_hlod_view(identity_of(404, 11, 5), 1, snap, spec).active());
	CHECK(mgr.hlod_bound_view_count() == 4);
	CHECK(mgr.select_hlod_view(b, 1, snap, spec).active()); // Refresh b: next_target is now the oldest.
	CHECK(mgr.select_hlod_view(identity_of(505, 12, 5), 1, snap, spec).admitted_now);
	CHECK(mgr.hlod_bound_view_count() == 4);
	CHECK(mgr.get_hlod_view_set().view_count() == 4);
	CHECK(mgr.select_hlod_view(next_target, 2, snap, spec).admitted_now); // It was evicted.
	CHECK(mgr.get_hlod_view_set().view_count() == 4);

	// An invalid identity never selects.
	out = mgr.select_hlod_view(HlodViewIdentity(), 1, snap, spec);
	CHECK_FALSE(out.active());
	CHECK(String(out.fallback.reason) == "complete_view_history_unavailable");

	mgr.clear_hlod_views();
	CHECK(mgr.hlod_bound_view_count() == 0);
	CHECK(mgr.get_hlod_view_set().view_count() == 0);
	CHECK(mgr.get_hlod_snapshot().assets.is_empty()); // The runtime snapshot is not kept alive.
}

TEST_CASE("[GaussianSplatting][HLOD] renderer policy change invalidates the refinement history through the view set") {
	auto tree = make_tree(IDENT);
	SelectionSnapshot snap = snapshot(tree, 0, 1000000);
	RenderFrameContextManager mgr;
	mgr.set_hlod_liveness_check(&all_live);
	ViewSpec spec = perspective(0);
	const HlodViewIdentity id = identity_of(101, 7, 5);
	auto nodes = [](const HlodViewOutcome &p_out) {
		Vector<uint32_t> v;
		for (uint32_t i = 0; i < p_out.record_count; ++i) { v.push_back(p_out.records[i].node); }
		return v;
	};
	HlodViewOutcome out = mgr.select_hlod_view(id, 1, snap, spec);
	CHECK(same(nodes(out), { 3, 4, 5, 6 }));
	// tau 22 is in the hysteresis band of the interiors: stale stamps would keep the leaves.
	spec.tau_pixels = 22;
	out = mgr.select_hlod_view(id, 1, snap, spec);
	CHECK_FALSE(out.admitted_now); // Invalidated in place, not by re-admission.
	CHECK(same(nodes(out), { 1, 2 }));
	RenderFrameContextManager cold;
	cold.set_hlod_liveness_check(&all_live);
	CHECK(same(nodes(cold.select_hlod_view(id, 1, snap, spec)), { 1, 2 }));
	// Unchanged policy keeps its history.
	out = mgr.select_hlod_view(id, 1, snap, spec);
	CHECK(same(nodes(out), { 1, 2 }));
}

TEST_CASE("[GaussianSplatting][HLOD] every non-ready view state falls back to the canonical route with an explicit reason") {
	using Cut = CutHistory;
	// Mapping table: nothing but READY is active, and every other state names its reason.
	ViewSet::ViewResult r;
	r.readiness = ViewSet::READY;
	CHECK(RenderFrameContextManager::hlod_fallback_for_view(r).reason == nullptr);
	struct Row {
		ViewSet::Readiness readiness;
		Cut::SelectStatus worst;
		bool exceeded;
		const char *reason;
		uint32_t bound;
		uint32_t dropped;
	};
	const Row rows[] = {
		{ ViewSet::LOADING, Cut::BOOTSTRAP_MISSING_RESIDENCY, false, "hlod_bootstrap_missing_residency", 1, 0 },
		{ ViewSet::LOADING, Cut::UNAVAILABLE, false, "cut_unavailable", 0, 0 },
		{ ViewSet::NOT_ADMITTED, Cut::BOOTSTRAP_INFEASIBLE, false, "hlod_bootstrap_infeasible", 1, 1 },
		{ ViewSet::NOT_ADMITTED, Cut::BOOTSTRAP_EXHAUSTED, false, "hlod_bootstrap_search_exhausted", 1, 0 },
		{ ViewSet::NOT_ADMITTED, Cut::COMPLETE, true, "hlod_view_reservation_exceeded", 1, 0 },
		{ ViewSet::NOT_ADMITTED, Cut::COMPLETE, false, "hlod_view_not_admitted", 1, 0 },
		{ ViewSet::FAILED, Cut::BOOTSTRAP_ARITHMETIC, false, "hlod_bootstrap_arithmetic_invalid", 0, 0 },
		{ ViewSet::FAILED, Cut::COMPLETE, false, "hlod_view_failed", 0, 0 },
		{ ViewSet::FAILED, Cut::INVALID, false, "hlod_view_failed", 0, 0 },
	};
	for (const Row &row : rows) {
		ViewSet::ViewResult v;
		v.readiness = row.readiness;
		v.worst_status = row.worst;
		v.reservation_exceeded = row.exceeded;
		const HlodFallback f = RenderFrameContextManager::hlod_fallback_for_view(v);
		REQUIRE(f.reason != nullptr);
		CHECK(String(f.reason) == String(row.reason));
		CHECK(f.budget_bound_frames == row.bound);
		CHECK(f.root_dropped_frames == row.dropped);
	}
	CHECK(RenderFrameContextManager::hlod_fallback_for_admission(ViewSet::ADMITTED).reason == nullptr);
	const ViewSet::AdmitStatus refusals[] = { ViewSet::REFUSED_INVALID, ViewSet::REFUSED_DUPLICATE, ViewSet::REFUSED_VIEW_LIMIT,
		ViewSet::REFUSED_CAPS, ViewSet::REFUSED_BUDGET, ViewSet::REFUSED_RESOURCES };
	for (ViewSet::AdmitStatus s : refusals) {
		CHECK(RenderFrameContextManager::hlod_fallback_for_admission(s).reason != nullptr);
	}
	CHECK(RenderFrameContextManager::hlod_fallback_for_snapshot(ViewSet::SNAPSHOT_OK).reason == nullptr);
	CHECK(String(RenderFrameContextManager::hlod_fallback_for_snapshot(ViewSet::SNAPSHOT_OVER_BUDGET).reason) == "hlod_snapshot_over_budget");
	CHECK(String(RenderFrameContextManager::hlod_fallback_for_snapshot(ViewSet::SNAPSHOT_INVALID).reason) == "hlod_snapshot_invalid");

	// End to end through the renderer entry, on a heavy-root tree that needs the bootstrap frontier.
	auto tree = make_tree(IDENT);
	tree->nodes.write[0].topology.payload_count = 100;
	tree->nodes.write[0].topology.radius = 50.0f;
	for (int i = 1; i < 3; ++i) { tree->nodes.write[i].topology.payload_count = 40; }
	SelectionSnapshot snap = snapshot(tree, 0, 1000000);
	snap.instances.write[0].caps = caps(90, 7, 1000);
	ViewSpec spec = perspective(0);
	spec.camera = Vector3(0, 0, 0);
	spec.tau_pixels = 1e9;
	spec.caps = caps(90, 7, 1000);
	RenderFrameContextManager mgr;
	mgr.set_hlod_liveness_check(&all_live);
	const HlodViewIdentity id = identity_of(101, 7, 5);
	HlodViewOutcome out = mgr.select_hlod_view(id, 1, snap, spec);
	CHECK(out.active());
	CHECK(out.record_count == 2);
	const uint64_t generation = out.view.draw_generation;

	// LOADING: a needed node loses residency. Not active, nothing published, no empty cut.
	Vector<uint8_t> resident;
	resident.resize(7);
	resident.fill(1);
	resident.write[2] = 0;
	snap.assets.write[0].resident = resident;
	out = mgr.select_hlod_view(id, 1, snap, spec);
	CHECK_FALSE(out.active());
	CHECK(out.view.readiness == ViewSet::LOADING);
	CHECK(String(out.fallback.reason) == "hlod_bootstrap_missing_residency");
	CHECK(out.records == nullptr);
	CHECK(out.record_count == 0);
	snap.assets.write[0].resident = Vector<uint8_t>();

	// NOT_ADMITTED: no complete frontier fits the instance splat cap.
	snap.instances.write[0].caps = caps(10, 7, 1000);
	out = mgr.select_hlod_view(id, 1, snap, spec);
	CHECK_FALSE(out.active());
	CHECK(out.view.readiness == ViewSet::NOT_ADMITTED);
	CHECK(String(out.fallback.reason) == "hlod_bootstrap_infeasible");
	CHECK(out.fallback.root_dropped_frames == 1);
	CHECK(out.records == nullptr);
	snap.instances.write[0].caps = caps(90, 7, 1000);

	// FAILED: invalid arithmetic input.
	ViewSpec nan_spec = spec;
	nan_spec.tau_pixels = std::numeric_limits<double>::quiet_NaN();
	out = mgr.select_hlod_view(id, 1, snap, nan_spec);
	CHECK_FALSE(out.active());
	CHECK(out.view.readiness == ViewSet::FAILED);
	CHECK(out.records == nullptr);

	// Recovery publishes a new complete set.
	out = mgr.select_hlod_view(id, 1, snap, spec);
	CHECK(out.active());
	CHECK(out.view.draw_generation > generation);

	// Outgrowing the reservation is repaired by one re-admission against the current snapshot.
	ViewSpec bigger = spec;
	bigger.caps = caps(200, 7, 1000);
	out = mgr.select_hlod_view(id, 1, snap, bigger);
	CHECK(out.active());
	CHECK(out.admitted_now);
	// ...and when the re-admission is refused the result is the explicit refusal, never an active cut.
	SelectionSnapshot tiny = snap;
	tiny.budget.physical_bytes = 10;
	ViewSpec biggest = spec;
	biggest.caps = caps(400, 7, 1000);
	out = mgr.select_hlod_view(id, 1, tiny, biggest);
	CHECK_FALSE(out.active());
	CHECK(String(out.fallback.reason) == "hlod_view_budget_refused");
	CHECK(mgr.hlod_bound_view_count() == 0);
	CHECK(mgr.get_hlod_view_set().view_count() == 0);
}

// ---- B3 review round ----
static Vector<uint64_t> g_live_buffers;
static bool test_liveness(const HlodViewIdentity &p_identity) {
	return g_live_buffers.has(uint64_t(p_identity.buffers));
}

TEST_CASE("[GaussianSplatting][HLOD] shrinking headroom keeps an affordable reservation active without re-admission") {
	auto tree = make_tree(IDENT);
	SelectionSnapshot snap = snapshot(tree, 0, 1000000);
	RenderFrameContextManager mgr;
	mgr.set_hlod_liveness_check(&all_live);
	ViewSpec spec = perspective(0);
	const HlodViewIdentity a = identity_of(101, 7, 5);
	const HlodViewIdentity b = identity_of(202, 8, 5);
	CHECK(mgr.select_hlod_view(a, 1, snap, spec).admitted_now);
	CHECK(mgr.select_hlod_view(b, 1, snap, spec).admitted_now);
	ViewSet::Ledger ledger;
	REQUIRE(mgr.get_hlod_view_set().ledger(snap, ledger));
	const uint64_t builds = mgr.get_hlod_view_set().tie_rank_builds();
	CHECK(builds == 1); // Both views share one rank table.
	// Headroom drops below the total of both reservations but stays above either one.
	snap.budget.physical_bytes = ledger.total() - 1;
	REQUIRE(snap.budget.physical_bytes >= ledger.view_bytes / 2);
	for (int frame = 0; frame < 3; ++frame) {
		HlodViewOutcome out = mgr.select_hlod_view(a, 1, snap, spec);
		CHECK(out.active());
		CHECK_FALSE(out.admitted_now);
		CHECK(out.view.draw_generation == uint64_t(2 + frame));
		out = mgr.select_hlod_view(b, 1, snap, spec);
		CHECK(out.active());
		CHECK_FALSE(out.admitted_now);
	}
	CHECK(mgr.get_hlod_view_set().tie_rank_builds() == builds); // No rank table was rebuilt.
	// A view that is genuinely unaffordable on its own is still withdrawn, explicitly.
	snap.budget.physical_bytes = ledger.view_bytes / 2 - 1;
	HlodViewOutcome out = mgr.select_hlod_view(a, 1, snap, spec);
	CHECK_FALSE(out.active());
	CHECK(String(out.fallback.reason) == "hlod_view_budget_refused");
	// Re-admitting unchanged content reuses the cached rank table.
	snap.budget.physical_bytes = 1000000;
	CHECK(mgr.select_hlod_view(a, 1, snap, spec).admitted_now);
	CHECK(mgr.get_hlod_view_set().tie_rank_builds() == builds);
	// New content revision builds a new table.
	tree->source_revision = 99;
	CHECK(mgr.select_hlod_view(a, 1, snap, spec).active());
	CHECK(mgr.get_hlod_view_set().tie_rank_builds() == builds + 1);
}

TEST_CASE("[GaussianSplatting][HLOD] bindings of freed buffers or stale epochs are evicted before a new view is refused") {
	auto tree = make_tree(IDENT);
	SelectionSnapshot snap = snapshot(tree, 0, 1000000);
	RenderFrameContextManager mgr;
	mgr.set_hlod_liveness_check(&test_liveness);
	g_live_buffers.clear();
	ViewSpec spec = perspective(0);
	for (uint64_t id = 1; id <= 4; ++id) {
		g_live_buffers.push_back(id);
		CHECK(mgr.select_hlod_view(identity_of(id, 10 + id, 5), 1, snap, spec).admitted_now);
	}
	CHECK(mgr.hlod_bound_view_count() == 4);
	// Buffers 4 (the most recently used) were freed. The LRU rule alone would evict buffers 1 instead.
	g_live_buffers.erase(4);
	CHECK(mgr.select_hlod_view(identity_of(5, 15, 5), 1, snap, spec).admitted_now);
	g_live_buffers.push_back(5);
	CHECK(mgr.hlod_bound_view_count() == 4);
	CHECK(mgr.get_hlod_view_set().view_count() == 4);
	CHECK_FALSE(mgr.select_hlod_view(identity_of(1, 11, 5), 1, snap, spec).admitted_now); // Not evicted.
	// Budget refusal case: room for exactly the live views; a freed one is released first.
	ViewSet::Ledger ledger;
	REQUIRE(mgr.get_hlod_view_set().ledger(snap, ledger));
	snap.budget.physical_bytes = ledger.total();
	g_live_buffers.erase(2);
	g_live_buffers.push_back(6);
	CHECK(mgr.select_hlod_view(identity_of(6, 16, 5), 1, snap, spec).admitted_now);
	CHECK(mgr.get_hlod_view_set().view_count() == 4);
	// A stale epoch is not live: with the check rejecting it, its reservation goes.
	mgr.set_hlod_liveness_check(nullptr); // Default check: these buffers do not exist in ObjectDB.
	CHECK(mgr.evict_stale_hlod_bindings() == 4);
	CHECK(mgr.get_hlod_view_set().view_count() == 0);
	mgr.set_hlod_liveness_check(&test_liveness);
}

TEST_CASE("[GaussianSplatting][HLOD] lifetime token epoch changes with configure and free_data and builds the view identity") {
	Ref<GaussianHlodViewLifetime> token;
	token.instantiate();
	HlodViewIdentity id;
	CHECK_FALSE(RenderFrameContextManager::hlod_identity_from_token(token.ptr(), ObjectID(uint64_t(9)), RID::from_uint64(3), id));
	Ref<RenderSceneBuffersRD> buffers;
	buffers.instantiate();
	token->configure(buffers.ptr());
	REQUIRE(RenderFrameContextManager::hlod_identity_from_token(token.ptr(), ObjectID(uint64_t(9)), RID::from_uint64(3), id));
	const uint64_t first = id.epoch;
	CHECK(first != 0);
	token->free_data();
	HlodViewIdentity after;
	REQUIRE(RenderFrameContextManager::hlod_identity_from_token(token.ptr(), ObjectID(uint64_t(9)), RID::from_uint64(3), after));
	CHECK(after.epoch != first);
	CHECK(RenderFrameContextManager::hlod_view_key(after) != RenderFrameContextManager::hlod_view_key(id));
	token->configure(nullptr);
	CHECK_FALSE(RenderFrameContextManager::hlod_identity_from_token(token.ptr(), ObjectID(uint64_t(9)), RID::from_uint64(3), after));
	CHECK_FALSE(RenderFrameContextManager::hlod_identity_from_token(nullptr, ObjectID(uint64_t(9)), RID::from_uint64(3), after));
	CHECK_FALSE(RenderFrameContextManager::hlod_identity_from_token(token.ptr(), ObjectID(), RID::from_uint64(3), after));
	// No render target on a fresh buffers object: no identity, and no token is attached.
	CHECK_FALSE(RenderFrameContextManager::acquire_hlod_identity(buffers.ptr(), after));
	CHECK_FALSE(RenderFrameContextManager::acquire_hlod_identity(nullptr, after));
}

TEST_CASE("[GaussianSplatting][HLOD] the reusable snapshot releases its runtime data reference") {
	auto tree = make_tree(IDENT);
	RenderFrameContextManager mgr;
	SelectionSnapshot &snap = mgr.get_hlod_snapshot();
	snap = snapshot(tree, 0, 1000000);
	CHECK(tree->get_reference_count() > 1);
	mgr.release_hlod_snapshot_data();
	CHECK(snap.assets[0].data.is_null());
	CHECK(tree->get_reference_count() == 1);
}
}
