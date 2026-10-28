extern "C" int test_hlod_view_set_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../lod/hlod_view_set.h"
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
// Box frustum x in [-4, 0]: an orthographic-style (parallel-plane) view that sees only the left half.
// The CPU oracle measures error by distance, so this exercises the view-volume, not an orthographic
// error model; that model is part of the deferred renderer wiring.
static ViewSpec orthographic(uint64_t p_key) {
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
// 100 splats * 4 + 1000 records * 1 + 8 nodes * 16.
static constexpr uint64_t VIEW_BYTES = 400 + 1000 + 128;

static Vector<uint32_t> nodes_of(const ViewSet &p_set, uint64_t p_view) {
	Vector<uint32_t> out;
	uint32_t n = 0;
	const DrawRecord *r = p_set.drawn(p_view, n);
	for (uint32_t i = 0; r && i < n; ++i) { out.push_back(r[i].node); }
	return out;
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
		CHECK(set.select(snap, results) == ViewSet::SNAPSHOT_OK);
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
	set.select(snap, results);
	uint32_t n = 0;
	const DrawRecord *r = set.drawn(1, n);
	if (!r || n == 0) { FAIL("no draw records"); return; }
	CHECK(r[0].version == (NodeVersion{ 8, 2 }));
	CHECK_FALSE(r[0].version == (NodeVersion{ 7, 1 }));
}

TEST_CASE("[GaussianSplatting][HLOD] perspective and orthographic views get independent complete cuts under one shared budget") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree, 1000, 1000 + 2 * VIEW_BYTES); // Exactly fits two views.
	ViewSet set;
	const auto p = set.admit_view(snap, perspective(1));
	const auto o = set.admit_view(snap, orthographic(2));
	CHECK(p.status == ViewSet::ADMITTED);
	CHECK(o.status == ViewSet::ADMITTED);
	CHECK(p.view_bytes == VIEW_BYTES);
	CHECK(o.charged_bytes == 1000 + VIEW_BYTES); // Payload once plus the first view.
	snap.views.push_back(perspective(1));
	snap.views.push_back(orthographic(2));
	Vector<ViewSet::ViewResult> results;
	CHECK(set.select(snap, results) == ViewSet::SNAPSHOT_OK);
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
	CHECK(ledger.view_bytes == 2 * VIEW_BYTES);
	CHECK(ledger.total() == snap.budget.physical_bytes); // At the boundary, not over it.
	// One byte less and the second view is refused, not squeezed in.
	auto tight = snapshot(tree, 1000, 1000 + 2 * VIEW_BYTES - 1);
	ViewSet tight_set;
	CHECK(tight_set.admit_view(tight, perspective(1)).status == ViewSet::ADMITTED);
	CHECK(tight_set.admit_view(tight, orthographic(2)).status == ViewSet::REFUSED_BUDGET);
	CHECK(tight_set.view_count() == 1);
}

TEST_CASE("[GaussianSplatting][HLOD] a third view that does not fit is refused explicitly and leaves existing cuts unchanged") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree, 1000, 1000 + 2 * VIEW_BYTES + VIEW_BYTES / 2);
	ViewSet set;
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	CHECK(set.admit_view(snap, orthographic(2)).status == ViewSet::ADMITTED);
	snap.views.push_back(perspective(1));
	snap.views.push_back(orthographic(2));
	Vector<ViewSet::ViewResult> before;
	set.select(snap, before);
	const Vector<uint32_t> cut1 = nodes_of(set, 1), cut2 = nodes_of(set, 2);
	const auto refusal = set.admit_view(snap, perspective(3));
	CHECK(refusal.status == ViewSet::REFUSED_BUDGET);
	CHECK(refusal.charged_bytes + refusal.view_bytes > refusal.budget_bytes);
	CHECK(set.view_count() == 2);
	uint32_t n = 0;
	CHECK(set.drawn(3, n) == nullptr);
	snap.views.push_back(perspective(3)); // An unadmitted view in the snapshot is ignored, not partly served.
	Vector<ViewSet::ViewResult> after;
	CHECK(set.select(snap, after) == ViewSet::SNAPSHOT_OK);
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
	second.transform.origin = Vector3(500, 0, 0);
	snap.instances.push_back(second); // Same asset, second placement.
	snap.instances.write[0].caps = caps(50, 4, 500);
	snap.instances.write[1].caps = caps(50, 4, 500);
	ViewSet set;
	ViewSet::Ledger ledger;
	CHECK(set.ledger(snap, ledger));
	CHECK(ledger.shared_payload_bytes == 1000); // Two instances, one payload.
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	CHECK(set.admit_view(snap, orthographic(2)).status == ViewSet::ADMITTED);
	CHECK(set.ledger(snap, ledger));
	CHECK(ledger.shared_payload_bytes == 1000); // Two views do not recharge it.
	CHECK(ledger.view_bytes == 2 * VIEW_BYTES); // Views are charged from their own caps, once each.
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
	CHECK(reference.admit_view(plain, orthographic(1)).status == ViewSet::ADMITTED);
	plain.views.push_back(orthographic(1));
	Vector<ViewSet::ViewResult> r0;
	reference.select(plain, r0);
	const Vector<uint32_t> expected = nodes_of(reference, 1);
	CHECK(same(expected, { 3, 4 }));
	// Place the instance (rotation + uniform scale 2 + translation) and move the same view with it.
	const Transform3D placement(Basis(Vector3(0, 1, 0), 0.7).scaled(Vector3(2, 2, 2)), Vector3(40, -3, 25));
	ViewSpec local = orthographic(1);
	ViewSpec world = local;
	world.camera = placement.xform(local.camera);
	for (int i = 0; i < 6; ++i) {
		const Vector3 point = local.frustum[i].normal * local.frustum[i].d;
		const Vector3 wp = placement.xform(point);
		const Vector3 wn = placement.basis.xform(local.frustum[i].normal).normalized();
		world.frustum[i] = Plane(wn, wn.dot(wp));
	}
	auto placed = snapshot(tree);
	placed.instances.write[0].transform = placement;
	ViewSet set;
	CHECK(set.admit_view(placed, world).status == ViewSet::ADMITTED);
	placed.views.push_back(world);
	Vector<ViewSet::ViewResult> r1;
	set.select(placed, r1);
	CHECK(same(nodes_of(set, 1), { 3, 4 })); // Scale and distance cancel: same cut as in the asset frame.
	// The unmoved world view no longer sees the placed instance the same way: transform is not ignored.
	ViewSet naive;
	auto moved_only = snapshot(tree);
	moved_only.instances.write[0].transform = placement;
	CHECK(naive.admit_view(moved_only, local).status == ViewSet::ADMITTED);
	moved_only.views.push_back(local);
	Vector<ViewSet::ViewResult> r2;
	naive.select(moved_only, r2);
	CHECK_FALSE(same(nodes_of(naive, 1), { 3, 4 }));
	// Non-uniform scale cannot keep error and distance consistent: refused up front.
	auto skew = snapshot(tree);
	skew.instances.write[0].transform.basis = Basis().scaled(Vector3(1, 2, 1));
	ViewSet refused;
	CHECK(refused.admit_view(skew, perspective(1)).status == ViewSet::REFUSED_INVALID);
}

TEST_CASE("[GaussianSplatting][HLOD] per-view publication is deterministic, order independent and reaches a static fixed point") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree, 1000, 100000);
	SnapshotInstance second = snap.instances[0];
	second.instance_key = 5; // Sorts before instance 11 whatever the listing order.
	second.transform.origin = Vector3(0.5, 0, 0);
	snap.instances.push_back(second);
	snap.instances.write[0].caps = caps(50, 4, 500);
	snap.instances.write[1].caps = caps(50, 4, 500);
	auto reversed = snap;
	reversed.instances.write[0] = snap.instances[1];
	reversed.instances.write[1] = snap.instances[0];
	for (auto *s : { &snap, &reversed }) {
		s->views.push_back(orthographic(2));
		s->views.push_back(perspective(1));
	}
	ViewSet forward, backward;
	CHECK(forward.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	CHECK(forward.admit_view(snap, orthographic(2)).status == ViewSet::ADMITTED);
	CHECK(backward.admit_view(reversed, orthographic(2)).status == ViewSet::ADMITTED); // Opposite admission order.
	CHECK(backward.admit_view(reversed, perspective(1)).status == ViewSet::ADMITTED);
	Vector<ViewSet::ViewResult> rf, rb;
	uint64_t previous_records[2] = { 0, 0 };
	Vector<DrawRecord> first_frame[2];
	for (int frame = 0; frame < 4; ++frame) {
		CHECK(forward.select(snap, rf) == ViewSet::SNAPSHOT_OK);
		CHECK(backward.select(reversed, rb) == ViewSet::SNAPSHOT_OK);
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
	ViewSpec other = orthographic(2);
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
	set.select(snap, results);
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
	set.select(snap, results);
	a = result_of(results, 1);
	CHECK(a->readiness == ViewSet::READY);
	CHECK(same(nodes_of(set, 1), { 1, 2 }));
	CHECK(a->draw_generation == 1);
	// Not admitted: no complete frontier fits the splat cap (leaves hold 16). The other view still fits.
	const Vector<uint32_t> before = nodes_of(set, 1);
	snap.instances.write[0].caps = caps(10, 7, 1000);
	set.select(snap, results);
	CHECK(result_of(results, 1)->readiness == ViewSet::NOT_ADMITTED);
	CHECK(result_of(results, 1)->worst_status == CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(nodes_of(set, 1) == before); // The last complete publication is kept but not reported as ready.
	CHECK(result_of(results, 1)->drawn_count == 0);
	CHECK(result_of(results, 2)->readiness == ViewSet::READY); // Its visible frontier (2 leaves, 8 splats) fits.
	snap.instances.write[0].caps = caps(90, 7, 1000);
	// Failed: invalid arithmetic input.
	snap.views.write[0].tau_pixels = std::numeric_limits<double>::quiet_NaN();
	set.select(snap, results);
	CHECK(result_of(results, 1)->readiness == ViewSet::FAILED);
	// Exceeding the reservation is explicit and selects nothing.
	snap.views.write[0].tau_pixels = 1e9;
	snap.views.write[0].caps = caps(200, 7, 1000);
	set.select(snap, results);
	CHECK(result_of(results, 1)->readiness == ViewSet::NOT_ADMITTED);
	CHECK(result_of(results, 1)->reservation_exceeded);
}

TEST_CASE("[GaussianSplatting][HLOD] a payload that outgrows the shared budget is reported without selecting or touching admitted views") {
	auto tree = make_tree(IDENT);
	auto snap = snapshot(tree, 1000, 1000 + VIEW_BYTES);
	ViewSet set;
	CHECK(set.admit_view(snap, perspective(1)).status == ViewSet::ADMITTED);
	snap.views.push_back(perspective(1));
	Vector<ViewSet::ViewResult> results;
	CHECK(set.select(snap, results) == ViewSet::SNAPSHOT_OK);
	const Vector<uint32_t> cut = nodes_of(set, 1);
	if (results.size() != 1) { FAIL("expected one result, got ", results.size()); return; }
	const uint64_t generation = results[0].draw_generation;
	snap.assets.write[0].payload_bytes = 1001; // The shared payload grew past the budget.
	CHECK(set.select(snap, results) == ViewSet::SNAPSHOT_OVER_BUDGET);
	if (results.size() != 1) { FAIL("expected one result, got ", results.size()); return; }
	CHECK(results[0].readiness == ViewSet::NOT_ADMITTED);
	CHECK(results[0].reservation_exceeded);
	CHECK(nodes_of(set, 1) == cut);
	snap.assets.write[0].payload_bytes = 1000;
	CHECK(set.select(snap, results) == ViewSet::SNAPSHOT_OK);
	if (results.size() != 1) { FAIL("expected one result, got ", results.size()); return; }
	CHECK(results[0].draw_generation == generation + 1);
	snap.instances.write[0].asset_index = 5; // Malformed snapshot.
	CHECK(set.select(snap, results) == ViewSet::SNAPSHOT_INVALID);
}
}
