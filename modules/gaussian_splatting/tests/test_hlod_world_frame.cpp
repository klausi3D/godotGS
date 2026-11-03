extern "C" int test_hlod_world_frame_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gaussian_splat_world_frame.h"
#include "../lod/hlod_view_set.h"
#include "../renderer/render_pipeline_stages.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/hlod_drawn_node.h"
#include "../renderer/tile_renderer.h"
#include "core/math/projection.h"
#include "scene/3d/node_3d.h"
#include <cmath>

// ADR 14.3 (package C): precise placement, origin revision, per-draw camera-relative offset. CPU only.
namespace TestHlodWorldFrame {
using namespace gs_world_frame;
using namespace gs_hlod_runtime;

static constexpr double OFFSETS[3] = { 0.0, 1000.0, 100000.0 };

static PreciseCoord coord(double p_x, double p_y, double p_z) {
	const double v[3] = { p_x, p_y, p_z };
	PreciseCoord c;
	CHECK(PreciseCoord::from_doubles(v, c));
	return c;
}
static Vector3 offset_vec(const PreciseCoord &p_a, const PreciseCoord &p_b) {
	double d[3];
	CHECK(p_a.offset_from(p_b, d));
	return Vector3(real_t(d[0]), real_t(d[1]), real_t(d[2]));
}
static double max_abs(const Vector3 &p_v) {
	return MAX(MAX(std::abs(double(p_v.x)), std::abs(double(p_v.y))), std::abs(double(p_v.z)));
}

TEST_CASE("[GaussianSplatting][HLOD] lattice coordinates are exact at any distance and normalised") {
	for (const double base : OFFSETS) {
		const double origin[3] = { base + 0.123456789012, -0.5 * base - 7.77, base };
		// Negative and positive indices, cells smaller and larger than the lattice edge.
		for (const int32_t e : { -2, 3, 10, 14 }) {
			for (const int64_t ix : { int64_t(-37), int64_t(0), int64_t(3), int64_t(4), int64_t(1001) }) {
				PreciseCoord a, b;
				REQUIRE(PreciseCoord::from_tree_cell_center(origin, e, ix, -ix, ix + 1, a));
				REQUIRE(PreciseCoord::from_tree_cell_center(origin, e, ix + 1, -ix, ix + 1, b));
				const double edge = std::ldexp(1.0, e);
				double d[3];
				REQUIRE(b.offset_from(a, d));
				CHECK(std::abs(d[0] - edge) <= 1e-9);
				CHECK(d[1] == 0.0);
				CHECK(d[2] == 0.0);
				for (int axis = 0; axis < 3; ++axis) {
					CHECK(a.local[axis] >= 0.0);
					CHECK(a.local[axis] < LATTICE_EDGE);
				}
			}
		}
		// translated() and offset_from() are inverse operations (within double rounding of the small delta).
		const PreciseCoord p = coord(origin[0], origin[1], origin[2]);
		const double delta[3] = { 3.21, -0.37, 4.13 };
		PreciseCoord q;
		REQUIRE(p.translated(delta, q));
		double back[3];
		REQUIRE(q.offset_from(p, back));
		for (int axis = 0; axis < 3; ++axis) { CHECK(std::abs(back[axis] - delta[axis]) <= 1e-9); }
	}
	// Non-finite and absurd input is refused, never wrapped.
	const double bad[3] = { std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0 };
	PreciseCoord out;
	CHECK_FALSE(PreciseCoord::from_doubles(bad, out));
	const double huge[3] = { 1.0e300, 0.0, 0.0 };
	CHECK_FALSE(PreciseCoord::from_doubles(huge, out));
	const double origin0[3] = { 0, 0, 0 };
	CHECK_FALSE(PreciseCoord::from_tree_cell_center(origin0, 60, 1, 0, 0, out));
}

TEST_CASE("[GaussianSplatting][HLOD] near-detail positions stay precise at 0, 1 km and 100 km; the naive float path fails at 100 km") {
	// A node centre and a camera 5 m away from it; a splat 1 cm from the centre. The camera-relative position of the
	// splat is q - d exactly, whatever the distance from the frame origin.
	const Vector3 q(0.0123f, -0.0045f, 0.0078f);
	const double d[3] = { 3.21, 0.37, -4.13 };
	double naive_error_at[3] = {};
	for (int k = 0; k < 3; ++k) {
		const double base = OFFSETS[k];
		const double tree_origin[3] = { base + 0.3, 2.0 * base + 0.4, -base + 0.6 };
		PreciseCoord center;
		REQUIRE(PreciseCoord::from_tree_cell_center(tree_origin, 7, 5, -3, 2, center));
		PreciseCoord camera;
		REQUIRE(center.translated(d, camera));
		const PrecisePlacement identity;
		Vector3 offset;
		REQUIRE(camera_relative_offset(identity, center, camera, offset));
		const Vector3 truth = q - Vector3(real_t(d[0]), real_t(d[1]), real_t(d[2]));
		const double error = max_abs((offset + q) - truth);
		// Stated bound: two float roundings of a value of about 5 m (2 x 4.8e-7 m).
		CHECK(error <= 2.0e-6);

		// Naive path: the absolute position rounded to float first (what a float world transform does).
		double c_abs[3], cam_abs[3];
		center.to_doubles(c_abs);
		camera.to_doubles(cam_abs);
		const Vector3 c_f = Vector3(real_t(c_abs[0]), real_t(c_abs[1]), real_t(c_abs[2]));
		const Vector3 cam_f = Vector3(real_t(cam_abs[0]), real_t(cam_abs[1]), real_t(cam_abs[2]));
		naive_error_at[k] = max_abs(((c_f + q) - cam_f) - truth);

		// Rotated (90 degrees about y) and uniformly scaled (x2) placement, exact in float. Basis columns map
		// (x, y, z) to (2z, 2y, -2x). The camera sits d away from the placed node centre: T + R s c + d.
		PrecisePlacement placed;
		placed.basis = Basis(Vector3(0, 0, -2), Vector3(0, 2, 0), Vector3(2, 0, 0));
		placed.translation = coord(base, 3.0, -9.0);
		double c_d[3];
		center.to_doubles(c_d);
		const double to_camera[3] = { 2.0 * c_d[2] + d[0], 2.0 * c_d[1] + d[1], -2.0 * c_d[0] + d[2] };
		PreciseCoord cam2;
		REQUIRE(placed.translation.translated(to_camera, cam2));
		Vector3 offset2;
		REQUIRE(camera_relative_offset(placed, center, cam2, offset2));
		// R s c + T - camera = -d.
		const Vector3 expected2(real_t(-d[0]), real_t(-d[1]), real_t(-d[2]));
		CHECK(max_abs(offset2 - expected2) <= 2.0e-6);
	}
	CHECK(naive_error_at[0] < 1.0e-4); // Fine near the origin, which is why the problem hides there.
	CHECK(naive_error_at[2] > 1.0e-3); // Fails at 100 km: this is the case the precise path exists for.
}

// ---- A seven node tree whose node centres are precise: 0 root; 1,2 interiors; 3..6 leaves. ----
static Ref<GaussianSplatHlodRuntimeData> make_tree(uint64_t p_revision = 7) {
	Ref<GaussianSplatHlodRuntimeData> data;
	data.instantiate();
	data->source_revision = p_revision;
	data->nodes.resize(7);
	const float x_of[7] = { 0, -2, 2, -3, -1, 1, 3 };
	for (uint32_t sem = 0; sem < 7; ++sem) {
		auto &n = data->nodes.write[sem];
		n.bounds_center = Vector3(x_of[sem], 0, 0);
		n.payload_center = Vector3(x_of[sem], 0, 0);
		n.precise_center = coord(x_of[sem], 0.0, 0.0);
		n.topology.radius = sem == 0 ? 4.5f : (sem < 3 ? 2.5f : 0.5f);
		n.topology.payload_count = sem < 3 ? 2 : 4;
		n.topology.geometric_error = sem == 0 ? 4.0f : (sem < 3 ? 2.0f : 0.0f);
		n.topology.payload_first = uint64_t(1000 + 37 * sem);
		n.topology.cell.e = sem == 0 ? 3 : (sem < 3 ? 2 : 1);
		n.topology.cell.ix = int64_t(sem) * 5 - 2;
		n.topology.cell.iy = -int64_t(sem);
		n.topology.cell.iz = 1;
		if (sem < 3) {
			n.topology.first_child = sem * 2 + 1;
			n.topology.child_count = 2;
		}
		if (sem != 0) { n.topology.parent = (sem - 1) / 2; }
	}
	return data;
}
static Caps caps() {
	Caps c;
	c.splat_budget = 100;
	c.node_budget = 8;
	c.record_budget = 1000;
	return c;
}
static const uint64_t ASSET = 0xA55E7;

// The placed asset at world position (base, 0, 0). The frame is described by `p_origin` and its revision.
static SelectionSnapshot make_snapshot(const Ref<GaussianSplatHlodRuntimeData> &p_tree, double p_base,
		const PreciseCoord &p_origin, uint64_t p_revision) {
	SelectionSnapshot s;
	s.hierarchy_revision = 1;
	s.origin = p_origin;
	s.origin_revision = p_revision;
	SnapshotAsset a;
	a.asset_key = ASSET;
	a.data = p_tree;
	a.bake_rule_revision = 1;
	a.payload_bytes = 1000;
	s.assets.push_back(a);
	SnapshotInstance inst;
	inst.instance_key = 11;
	inst.asset_index = 0;
	inst.placement.translation = coord(p_base, 0, 0);
	inst.caps = caps();
	s.instances.push_back(inst);
	s.budget.physical_bytes = 100000;
	s.budget.sort_bytes_per_splat = 4;
	s.budget.record_bytes_per_record = 1;
	s.budget.draw_bytes_per_node = 16;
	s.budget.max_views = 8;
	return s;
}
// A view whose camera sits at world (base, 0, z): frame coordinates are formed in double from the precise origin.
static ViewSpec perspective_at(uint64_t p_key, double p_base, double p_z, const PreciseCoord &p_origin) {
	ViewSpec v;
	v.view_key = p_key;
	v.camera = offset_vec(coord(p_base, 0, p_z), p_origin);
	v.frustum_enabled = false;
	v.focal_pixels = 100;
	v.tau_pixels = 1;
	v.caps = caps();
	return v;
}
// Box frustum x in [-4, 0] around the asset, in frame space.
static ViewSpec box_at(uint64_t p_key, double p_base, double p_z, const PreciseCoord &p_origin) {
	ViewSpec v = perspective_at(p_key, p_base, p_z, p_origin);
	v.frustum_enabled = true;
	const Vector3 normals[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
	const double d0[6] = { 0, 4, 10, 10, 100, 100 };
	for (int i = 0; i < 6; ++i) {
		const Vector3 local_point = normals[i] * real_t(d0[i]);
		const Vector3 frame_point = offset_vec(coord(p_base + local_point.x, local_point.y, local_point.z), p_origin);
		v.frustum[i] = Plane(normals[i], normals[i].dot(frame_point));
	}
	return v;
}

struct DrawCopy {
	Vector<uint32_t> nodes;
	Vector<NodeKey> keys;
	Vector<Vector3> offsets;
	uint64_t revision = 0;
};
static DrawCopy copy_draw(const ViewSet &p_set, uint64_t p_view) {
	DrawCopy out;
	uint32_t n = 0;
	const DrawRecord *r = p_set.drawn(p_view, n);
	for (uint32_t i = 0; r && i < n; ++i) {
		out.nodes.push_back(r[i].node);
		out.keys.push_back(r[i].key);
		out.offsets.push_back(r[i].camera_offset);
	}
	out.revision = p_set.drawn_origin_revision(p_view);
	return out;
}
static ViewSet::ViewResult select_one(ViewSet &p_set, SelectionSnapshot &p_snapshot, const ViewSpec &p_view) {
	p_snapshot.views.clear();
	p_snapshot.views.push_back(p_view);
	CHECK(p_set.select(p_snapshot) == ViewSet::SNAPSHOT_OK);
	uint32_t n = 0;
	const ViewSet::ViewResult *r = p_set.results(n);
	CHECK(n == 1);
	return n ? r[0] : ViewSet::ViewResult();
}
static bool same_nodes(const Vector<uint32_t> &p_a, std::initializer_list<uint32_t> p_b) {
	if (uint32_t(p_a.size()) != p_b.size()) { return false; }
	int i = 0;
	for (uint32_t v : p_b) { if (p_a[i++] != v) { return false; } }
	return true;
}

TEST_CASE("[GaussianSplatting][HLOD] repeated origin shifts keep NodeKeys, node payloads, ties and offsets") {
	const double base = OFFSETS[2];
	auto tree = make_tree();
	// Snapshot of the node-local payload fields before any shift.
	Vector<Vector3> payload_before, bounds_before;
	Vector<PreciseCoord> precise_before;
	Vector<uint64_t> first_before;
	for (int i = 0; i < tree->nodes.size(); ++i) {
		payload_before.push_back(tree->nodes[i].payload_center);
		bounds_before.push_back(tree->nodes[i].bounds_center);
		precise_before.push_back(tree->nodes[i].precise_center);
		first_before.push_back(tree->nodes[i].topology.payload_first);
	}
	struct NodeGuard {
		Node3D *node = memnew(Node3D);
		~NodeGuard() { memdelete(node); }
	} guard;
	Node3D *unrelated = guard.node;
	const Transform3D scene_transform(Basis(Vector3(0, 1, 0), 0.5f), Vector3(12.5f, -3.0f, 99.0f));
	unrelated->set_transform(scene_transform);

	WorldOriginContext ctx;
	ViewSet set;
	SelectionSnapshot snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
	const ViewSpec first_spec = perspective_at(1, base, 10.0, ctx.get_origin());
	REQUIRE(set.admit_view(snap, first_spec).status == ViewSet::ADMITTED);
	const ViewSet::ViewResult first = select_one(set, snap, first_spec);
	CHECK(first.readiness == ViewSet::READY);
	const DrawCopy baseline = copy_draw(set, 1);
	CHECK(same_nodes(baseline.nodes, { 3, 4, 5, 6 }));
	CHECK(baseline.revision == 0);

	// Origins near the camera (the camera is a float in the frame, so the frame must stay close to it), visited
	// repeatedly and out of order, including a return to an earlier origin.
	const double shifts[][3] = { { 50, 0, 0 }, { -20, 3, 7 }, { 33.25, -8, 0.5 }, { 50, 0, 0 }, { 0, 0, 0 }, { -61.5, 2, -3 },
		{ 7, 7, 7 }, { 50, 0, 0 }, { 0, 0, 0 } };
	uint64_t expected_revision = 0;
	for (const auto &s : shifts) {
		const PreciseCoord next = coord(base + s[0], s[1], s[2]);
		const bool changes = next != ctx.get_origin();
		REQUIRE(ctx.set_origin(next));
		expected_revision += changes ? 1 : 0;
		CHECK(ctx.get_revision() == expected_revision);
		snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
		const ViewSpec spec = perspective_at(1, base, 10.0, ctx.get_origin());
		const ViewSet::ViewResult r = select_one(set, snap, spec);
		CHECK(r.readiness == ViewSet::READY);
		CHECK(r.origin_changed == changes);
		CHECK(r.origin_revision == ctx.get_revision());
		const DrawCopy now = copy_draw(set, 1);
		CHECK(now.revision == ctx.get_revision());
		CHECK(now.nodes == baseline.nodes);
		if (now.keys.size() != baseline.keys.size() || now.offsets.size() != baseline.offsets.size()) {
			FAIL("draw record count changed after an origin shift");
			return;
		}
		for (int i = 0; i < now.keys.size(); ++i) {
			CHECK(now.keys[i] == baseline.keys[i]);
			// Camera-relative offsets do not depend on the origin; only the float camera in the frame limits them.
			CHECK(max_abs(now.offsets[i] - baseline.offsets[i]) <= 1.0e-5);
		}
	}
	// Nothing stored in the node-local payload moved, and the unrelated scene node was never touched.
	for (int i = 0; i < tree->nodes.size(); ++i) {
		CHECK(tree->nodes[i].payload_center == payload_before[i]);
		CHECK(tree->nodes[i].bounds_center == bounds_before[i]);
		CHECK(tree->nodes[i].precise_center == precise_before[i]);
		CHECK(tree->nodes[i].topology.payload_first == first_before[i]);
	}
	CHECK(unrelated->get_transform() == scene_transform);
}

TEST_CASE("[GaussianSplatting][HLOD] origin context bumps its revision on every change and notifies hooks") {
	struct Recorder : OriginShiftHook {
		Vector<uint64_t> revisions;
		PreciseCoord last_old, last_new;
		void on_world_origin_changed(const PreciseCoord &p_old, const PreciseCoord &p_new, uint64_t p_revision) override {
			revisions.push_back(p_revision);
			last_old = p_old;
			last_new = p_new;
		}
	} recorder;
	WorldOriginContext ctx;
	ctx.add_hook(&recorder);
	ctx.add_hook(&recorder); // Registered once.
	CHECK(ctx.get_revision() == 0);
	CHECK(ctx.set_origin(PreciseCoord())); // No change: no bump, no notification.
	CHECK(ctx.get_revision() == 0);
	CHECK(recorder.revisions.is_empty());
	const double delta[3] = { 100000.5, 0.0, -2048.25 };
	CHECK(ctx.shift_origin_by(delta));
	CHECK(ctx.get_revision() == 1);
	CHECK(ctx.shift_origin_by(delta));
	CHECK(ctx.get_revision() == 2);
	if (recorder.revisions.size() != 2) {
		FAIL("expected two notifications, got ", recorder.revisions.size());
		return;
	}
	CHECK(recorder.revisions[0] == 1);
	CHECK(recorder.revisions[1] == 2);
	CHECK(recorder.last_new == ctx.get_origin());
	// A representable-looking but malformed origin is refused without a bump.
	PreciseCoord malformed;
	malformed.local[0] = LATTICE_EDGE;
	CHECK_FALSE(ctx.set_origin(malformed));
	CHECK(ctx.get_revision() == 2);
	// Going back to an earlier origin is still a change.
	CHECK(ctx.set_origin(PreciseCoord()));
	CHECK(ctx.get_revision() == 3);
	ctx.remove_hook(&recorder);
	CHECK(ctx.shift_origin_by(delta));
	CHECK(recorder.revisions.size() == 3);
}

TEST_CASE("[GaussianSplatting][HLOD] an origin revision change drops hysteresis and the published set before reselecting") {
	const double base = OFFSETS[2];
	auto tree = make_tree();
	// Root error at z = 360: 4 * 100 / (360 - 4.5) = 1.125 px, inside the (tau, 1.25 tau] hysteresis band. Whether the root
	// is refined there depends only on the history of the view: refined while near, kept while in the band.
	const double near_z = 14.0, band_z = 360.0;
	WorldOriginContext ctx;
	ViewSet set;
	SelectionSnapshot snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
	REQUIRE(set.admit_view(snap, perspective_at(1, base, near_z, ctx.get_origin())).status == ViewSet::ADMITTED);
	CHECK(select_one(set, snap, perspective_at(1, base, near_z, ctx.get_origin())).readiness == ViewSet::READY);
	CHECK(same_nodes(copy_draw(set, 1).nodes, { 3, 4, 5, 6 }));
	// Control: same frame, same revision: hysteresis keeps the root refined.
	snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
	const ViewSet::ViewResult kept = select_one(set, snap, perspective_at(1, base, band_z, ctx.get_origin()));
	CHECK_FALSE(kept.origin_changed);
	CHECK(same_nodes(copy_draw(set, 1).nodes, { 1, 2 }));
	// Same world camera, but the origin moved: the previous histories belong to the old frame and are dropped, so
	// the root is no longer held refined.
	REQUIRE(ctx.set_origin(coord(base, 0, 300)));
	snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
	const ViewSet::ViewResult moved = select_one(set, snap, perspective_at(1, base, band_z, ctx.get_origin()));
	CHECK(moved.origin_changed);
	CHECK(same_nodes(copy_draw(set, 1).nodes, { 0 }));
	CHECK(set.drawn_origin_revision(1) == ctx.get_revision());

	// A changed revision whose selection cannot complete leaves no stale set behind (nothing mixes old offsets).
	REQUIRE(ctx.set_origin(coord(base, 0, 100)));
	snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
	snap.assets.write[0].resident.resize(7);
	snap.assets.write[0].resident.fill(0);
	const ViewSet::ViewResult loading = select_one(set, snap, perspective_at(1, base, near_z, ctx.get_origin()));
	CHECK(loading.readiness == ViewSet::LOADING);
	uint32_t n = 99;
	CHECK(set.drawn(1, n) == nullptr);
	CHECK(n == 0);
	CHECK(set.drawn_origin_revision(1) == 0);
}

TEST_CASE("[GaussianSplatting][HLOD] a return to origin 0 under a new revision still drops the histories") {
	// 0 -> A -> 0 where the A frame is never selected (the renderer falls back before select()): the revision is 2,
	// the origin equals the one the histories were built under, and they must still be dropped.
	const double base = OFFSETS[2];
	auto tree = make_tree();
	WorldOriginContext ctx;
	ViewSet set;
	SelectionSnapshot snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
	REQUIRE(set.admit_view(snap, perspective_at(1, base, 14.0, ctx.get_origin())).status == ViewSet::ADMITTED);
	CHECK(select_one(set, snap, perspective_at(1, base, 14.0, ctx.get_origin())).readiness == ViewSet::READY);
	snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
	select_one(set, snap, perspective_at(1, base, 360.0, ctx.get_origin()));
	CHECK(same_nodes(copy_draw(set, 1).nodes, { 1, 2 })); // Control: hysteresis holds under an unchanged revision.
	REQUIRE(ctx.set_origin(coord(base, 0, 300)));
	REQUIRE(ctx.set_origin(PreciseCoord()));
	CHECK(ctx.get_revision() == 2);
	CHECK(ctx.get_origin() == PreciseCoord());
	snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
	const ViewSet::ViewResult back = select_one(set, snap, perspective_at(1, base, 360.0, ctx.get_origin()));
	CHECK(back.origin_changed);
	CHECK(same_nodes(copy_draw(set, 1).nodes, { 0 }));
	CHECK(set.drawn_origin_revision(1) == 2);
}

TEST_CASE("[GaussianSplatting][HLOD] a float-only camera is limited by its float; a precise camera is not") {
	const double base = OFFSETS[2];
	auto tree = make_tree();
	const PreciseCoord far_origin; // Origin 0, camera and asset 100 km away: the float camera is already rounded.
	const PreciseCoord near_origin = coord(base, 0, 0); // Shifting the origin near the camera keeps it precise.
	// Node 3 sits at asset x = -3 (placed at x = base); the camera is at base + 0.3. Offset x is -3.3, z is -10.
	const Vector3 truth(-3.3f, 0, -10);
	for (int mode = 0; mode < 3; ++mode) { // 0 float, origin 0; 1 precise, origin 0; 2 float, near origin.
		const PreciseCoord &origin = mode == 2 ? near_origin : far_origin;
		SelectionSnapshot snap = make_snapshot(tree, base, origin, 0);
		ViewSpec spec = perspective_at(1, base, 10.0, origin);
		const PreciseCoord world_camera = coord(base + 0.3, 0, 10);
		spec.camera = offset_vec(world_camera, origin);
		spec.camera_precise = world_camera;
		spec.camera_precise_valid = mode == 1;
		ViewSet set;
		REQUIRE(set.admit_view(snap, spec).status == ViewSet::ADMITTED);
		CHECK(select_one(set, snap, spec).readiness == ViewSet::READY);
		const DrawCopy d = copy_draw(set, 1);
		if (d.nodes.is_empty() || d.nodes[0] != 3) {
			FAIL("expected node 3 first");
			return;
		}
		const double error = max_abs(d.offsets[0] - truth);
		if (mode == 0) {
			CHECK(error > 1.0e-3); // Float camera 100 km out: millimetres lost.
		} else {
			CHECK(error <= 2.0e-6);
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD] unload and reload at 0, 1 km and 100 km reproduce keys and camera-relative offsets") {
	DrawCopy reference;
	for (int k = 0; k < 3; ++k) {
		const double base = OFFSETS[k];
		WorldOriginContext ctx;
		REQUIRE(ctx.set_origin(coord(base, 0, 0)));
		DrawCopy first, second;
		ViewSet::Ledger ledger_first, ledger_second;
		{
			auto tree = make_tree();
			ViewSet set;
			SelectionSnapshot snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
			const ViewSpec spec = perspective_at(1, base, 10.0, ctx.get_origin());
			REQUIRE(set.admit_view(snap, spec).status == ViewSet::ADMITTED);
			CHECK(select_one(set, snap, spec).readiness == ViewSet::READY);
			first = copy_draw(set, 1);
			CHECK(set.ledger(snap, ledger_first));
			// Unload: the view and the payload go away.
			CHECK(set.remove_view(1));
			uint32_t count = 7;
			CHECK(set.drawn(1, count) == nullptr);
			CHECK(count == 0);
			// Reload into the same set from a freshly built payload.
			auto reloaded = make_tree();
			SelectionSnapshot snap2 = make_snapshot(reloaded, base, ctx.get_origin(), ctx.get_revision());
			REQUIRE(set.admit_view(snap2, spec).status == ViewSet::ADMITTED);
			CHECK(select_one(set, snap2, spec).readiness == ViewSet::READY);
			second = copy_draw(set, 1);
			CHECK(set.ledger(snap2, ledger_second));
		}
		CHECK(first.nodes == second.nodes);
		if (first.keys.size() != second.keys.size() || first.offsets.size() != second.offsets.size() ||
				first.keys.size() != first.offsets.size()) {
			FAIL("draw record count changed across unload/reload");
			return;
		}
		for (int i = 0; i < first.keys.size(); ++i) {
			CHECK(first.keys[i] == second.keys[i]);
			CHECK(first.offsets[i] == second.offsets[i]); // Bit-identical.
		}
		CHECK(ledger_first.total() == ledger_second.total());
		if (k == 0) {
			reference = first;
		} else {
			// The same placement 1 km / 100 km away from the world zero selects the same nodes with the same offsets.
			if (first.nodes != reference.nodes || first.offsets.size() != reference.offsets.size() ||
					first.keys.size() != reference.keys.size()) {
				FAIL("selection differs from the zero-offset result");
				return;
			}
			for (int i = 0; i < first.offsets.size(); ++i) {
				CHECK(first.keys[i] == reference.keys[i]);
				CHECK(max_abs(first.offsets[i] - reference.offsets[i]) <= 1.0e-5);
			}
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD] a perspective and a box view share a 100 km frame and agree with the zero-offset result") {
	Vector<uint32_t> persp_nodes, box_nodes;
	Vector<Vector3> persp_offsets, box_offsets;
	for (int k = 0; k < 3; ++k) {
		const double base = OFFSETS[k];
		WorldOriginContext ctx;
		REQUIRE(ctx.set_origin(coord(base, 0, 0)));
		auto tree = make_tree();
		SelectionSnapshot snap = make_snapshot(tree, base, ctx.get_origin(), ctx.get_revision());
		const ViewSpec persp = perspective_at(1, base, 10.0, ctx.get_origin());
		const ViewSpec box = box_at(2, base, 10.0, ctx.get_origin());
		ViewSet set;
		REQUIRE(set.admit_view(snap, persp).status == ViewSet::ADMITTED);
		REQUIRE(set.admit_view(snap, box).status == ViewSet::ADMITTED);
		snap.views.clear();
		snap.views.push_back(persp);
		snap.views.push_back(box);
		CHECK(set.select(snap) == ViewSet::SNAPSHOT_OK);
		const DrawCopy p = copy_draw(set, 1);
		const DrawCopy b = copy_draw(set, 2);
		CHECK(same_nodes(p.nodes, { 3, 4, 5, 6 }));
		CHECK(same_nodes(b.nodes, { 3, 4 }));
		CHECK(p.revision == ctx.get_revision());
		CHECK(b.revision == ctx.get_revision());
		// Both views look from the same camera, so shared nodes carry the same offset.
		CHECK(p.offsets[0] == b.offsets[0]);
		CHECK(p.offsets[1] == b.offsets[1]);
		if (k == 0) {
			persp_nodes = p.nodes;
			box_nodes = b.nodes;
			persp_offsets = p.offsets;
			box_offsets = b.offsets;
		} else {
			if (p.nodes != persp_nodes || b.nodes != box_nodes || p.offsets.size() != persp_offsets.size() ||
					b.offsets.size() != box_offsets.size()) {
				FAIL("selection differs from the zero-offset result");
				return;
			}
			for (int i = 0; i < p.offsets.size(); ++i) { CHECK(max_abs(p.offsets[i] - persp_offsets[i]) <= 1.0e-5); }
			for (int i = 0; i < b.offsets.size(); ++i) { CHECK(max_abs(b.offsets[i] - box_offsets[i]) <= 1.0e-5); }
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD] the cull-config reuse signature follows the origin revision") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	GaussianSplatSceneDirector *owned = nullptr;
	if (!director) {
		owned = memnew(GaussianSplatSceneDirector);
		director = owned;
	}
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	if (renderer.is_null()) {
		FAIL("GaussianSplatRenderer must instantiate");
		return;
	}
	GaussianSplatRenderer::FrameStateProvider provider(renderer.ptr());
	auto signature = [&]() { return RenderPipelineStages::compute_cull_config_signature(*renderer.ptr(), provider); };
	PreciseCoord saved_origin;
	uint64_t saved_revision = 0;
	director->get_world_origin(saved_origin, saved_revision);
	const uint64_t before = signature();
	CHECK(signature() == before); // Deterministic, otherwise every inequality below is vacuous.
	const PreciseCoord moved = coord(100000.0, 0.0, 0.0);
	REQUIRE(director->set_world_origin(moved));
	const uint64_t after = signature();
	CHECK(after != before);
	CHECK(director->set_world_origin(moved)); // Unchanged origin: unchanged revision, unchanged signature.
	CHECK(signature() == after);
	// Back to the earlier origin is a new revision, so a render cached under it is not reused either.
	REQUIRE(director->set_world_origin(saved_origin));
	CHECK(signature() != before);
	CHECK(signature() != after);
	if (owned) { memdelete(owned); }
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO] a baked world far from the origin keeps its cell centres in the precise frame") {
	Ref<GaussianData> data;
	data.instantiate();
	LocalVector<Gaussian> gaussians;
	gaussians.resize(8);
	for (uint32_t i = 0; i < 8; ++i) {
		// Exactly representable float positions at about 100 km (float spacing there is 1/128 m).
		gaussians[i].position = Vector3(100000.0f + i * 0.0625f, 0.125f + i * 0.03125f, -100000.0f + i * 0.125f);
		gaussians[i].scale = Vector3(0.03125f, 0.0625f, 0.125f);
		gaussians[i].opacity = 0.5f;
	}
	LocalVector<Vector3> sh;
	data->set_gaussian_payload(gaussians, sh, 0u, 0u, false, false);
	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_gaussian_data(data);
	if (world->bake_hlod() != OK) { FAIL("synthetic world bake failed"); return; }
	Ref<GaussianSplatHlodRuntimeData> snapshot;
	String reason;
	if (world->capture_hlod_runtime_snapshot(snapshot, reason) != OK || snapshot.is_null()) {
		FAIL("snapshot failed: ", reason.utf8().get_data());
		return;
	}
	double worst_precise = 0.0;
	bool any_node_checked = false;
	for (int i = 0; i < snapshot->nodes.size(); ++i) {
		const auto &n = snapshot->nodes[i];
		double precise[3];
		n.precise_center.to_doubles(precise);
		for (int a = 0; a < 3; ++a) {
			worst_precise = MAX(worst_precise, std::abs(precise[a] - n.exact_cell_center[a]));
			any_node_checked = true;
		}
	}
	// The precise centre is the source cell centre (double rounding of its own to_doubles only), taken from the tree
	// origin and the integer lattice address and not from the float payload_center.
	CHECK(any_node_checked);
	CHECK(worst_precise <= 1.0e-9);
}

// ---------------------------------------------------------------------------------------------------------------
// Package C2: the per-draw GPU record, its shader mirror, and the origin-revision keys of the HLOD route's caches.
// ---------------------------------------------------------------------------------------------------------------

// CPU mirror of gs_hlod_camera_relative_position (shaders/includes/gs_camera_relative.glsl) for a placement with a
// rotation/scale basis; the GLSL itself is evaluated against the same reference in tests/ci/test_shader_host_numerics.py.
static Vector3 mirror_camera_relative_position(const Basis &p_basis, const Vector3 &p_node_position, const Vector3 &p_offset) {
	return p_basis.xform(p_node_position) + p_offset;
}

TEST_CASE("[GaussianSplatting][HLOD][C2] the per-draw GPU record is 32 B and carries the double-formed offset at 0, 1 km and 100 km") {
	CHECK(sizeof(DrawnNodeGPU) == 32);
	CHECK(offsetof(DrawnNodeGPU, instance_id) == 0);
	CHECK(offsetof(DrawnNodeGPU, chunk_id) == 4);
	CHECK(offsetof(DrawnNodeGPU, fade) == 8);
	CHECK(offsetof(DrawnNodeGPU, camera_offset) == 16);
	auto tree = make_tree();
	const float x_of[7] = { 0, -2, 2, -3, -1, 1, 3 };
	const Vector3 local_point(0.5f, -0.25f, 1.0f); // Node-relative splat position (shared payload).
	for (const double base : OFFSETS) {
		for (const bool origin_near_camera : { false, true }) {
			// Origin at the world origin (precise camera needed past the float) or shifted next to the camera.
			const PreciseCoord origin = origin_near_camera ? coord(base, 0, 0) : PreciseCoord();
			SelectionSnapshot snap = make_snapshot(tree, base, origin, origin_near_camera ? 1 : 0);
			ViewSpec spec = perspective_at(1, base, 10.0, origin);
			const PreciseCoord world_camera = coord(base + 0.3, 0, 10);
			spec.camera = offset_vec(world_camera, origin);
			spec.camera_precise = world_camera;
			spec.camera_precise_valid = true;
			ViewSet set;
			REQUIRE(set.admit_view(snap, spec).status == ViewSet::ADMITTED);
			CHECK(select_one(set, snap, spec).readiness == ViewSet::READY);
			uint32_t count = 0;
			const DrawRecord *records = set.drawn(1, count);
			REQUIRE(records != nullptr);
			REQUIRE(count > 0);
			double worst = 0.0, worst_naive = 0.0;
			for (uint32_t i = 0; i < count; ++i) {
				const DrawnNodeGPU gpu = gs_hlod_gpu::make_drawn_node(0u, records[i].node, records[i].camera_offset);
				CHECK(gpu.instance_id == 0u);
				CHECK(gpu.chunk_id == records[i].node);
				CHECK(gpu.fade == 1.0f);
				CHECK(gpu.reserved == 0u);
				CHECK(gpu.camera_offset[3] == 0.0f);
				// Double reference: world node centre (asset x + placement base) minus the camera, no float absolute.
				const double node_x = double(x_of[records[i].node]);
				const double ref[3] = { node_x - 0.3, 0.0, -10.0 };
				for (int a = 0; a < 3; ++a) { worst = MAX(worst, std::abs(double(gpu.camera_offset[a]) - ref[a])); }
				// Shader mirror: R s p + offset against the double truth of the splat's camera-relative position.
				const Vector3 got = mirror_camera_relative_position(Basis(), local_point,
						Vector3(gpu.camera_offset[0], gpu.camera_offset[1], gpu.camera_offset[2]));
				const double truth[3] = { ref[0] + double(local_point.x), ref[1] + double(local_point.y), ref[2] + double(local_point.z) };
				for (int a = 0; a < 3; ++a) { worst = MAX(worst, std::abs(double(got[a]) - truth[a])); }
				// The base formulation: absolute float position first, float camera last.
				const float abs_x = float(base + node_x + double(local_point.x));
				worst_naive = MAX(worst_naive, std::abs(double(abs_x - float(base + 0.3)) - truth[0]));
			}
			CHECK(worst <= 3.0e-6);
			if (base >= OFFSETS[2]) {
				CHECK(worst_naive > 1.0e-3); // The absolute float path must fail where the record passes.
			}
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][C2] a rotated and scaled placement folds R s c + T - camera into the record at 100 km") {
	const double base = OFFSETS[2];
	const Basis basis = Basis(Vector3(0, 1, 0), 0.7f).scaled(Vector3(2, 2, 2));
	PrecisePlacement placement;
	placement.basis = basis;
	const Vector3 node_centre(3.25f, 7.5f, 1.125f); // Asset-space centre, exact in float.
	const PreciseCoord centre = coord(node_centre.x, node_centre.y, node_centre.z);
	// Place the instance so the node lands 2 m / -1 m / -6 m from the camera.
	const Vector3 rs_c = basis.xform(node_centre);
	const double cam[3] = { base + 0.3, 1.0, 9.0 };
	placement.translation = coord(cam[0] + 2.0 - double(rs_c.x), cam[1] - 1.0 - double(rs_c.y), cam[2] - 6.0 - double(rs_c.z));
	Vector3 offset;
	REQUIRE(camera_relative_offset(placement, centre, coord(cam[0], cam[1], cam[2]), offset));
	const DrawnNodeGPU gpu = gs_hlod_gpu::make_drawn_node(3u, 5u, offset);
	const Vector3 local_point(1.5f, -2.25f, 0.125f);
	const Vector3 got = mirror_camera_relative_position(basis, local_point, Vector3(gpu.camera_offset[0], gpu.camera_offset[1], gpu.camera_offset[2]));
	// Truth in double: R s (c + p) + T - camera, with R s taken from the basis entries as doubles.
	double world[3], t[3];
	placement.translation.to_doubles(t);
	const double in[3] = { double(node_centre.x) + double(local_point.x), double(node_centre.y) + double(local_point.y), double(node_centre.z) + double(local_point.z) };
	for (int r = 0; r < 3; ++r) {
		world[r] = double(basis[r][0]) * in[0] + double(basis[r][1]) * in[1] + double(basis[r][2]) * in[2] + t[r];
	}
	for (int a = 0; a < 3; ++a) { CHECK(std::abs(double(got[a]) - (world[a] - cam[a])) <= 4.0e-6); }
	CHECK(gpu.instance_id == 3u);
	CHECK(gpu.chunk_id == 5u);
}

TEST_CASE("[GaussianSplatting][HLOD][C2] the depth pass frustum planes are camera-translated and classify borderline points at 100 km") {
	Projection projection;
	projection.set_perspective(60.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
	const double base = OFFSETS[2];
	const Transform3D camera_to_world(Basis(Vector3(0, 1, 0), 0.6f) * Basis(Vector3(1, 0, 0), -0.2f), Vector3(real_t(base + 0.3), 0.0f, 10.0f));
	const Vector<Plane> relative = gs_hlod_gpu::camera_relative_frustum_planes(projection, camera_to_world);
	const Vector<Plane> absolute = projection.get_projection_planes(camera_to_world);
	if (relative.size() != 6 || absolute.size() != 6) {
		FAIL("expected six frustum planes, got ", relative.size(), " and ", absolute.size());
		return;
	}
	int relative_wrong = 0, absolute_wrong = 0, checked = 0;
	for (int i = 0; i < 6; ++i) {
		const Vector3 n = relative[i].normal;
		// Pass-through-the-camera planes carry no camera distance: the four side planes have d == 0 (float noise only).
		if (i == Projection::PLANE_LEFT || i == Projection::PLANE_RIGHT || i == Projection::PLANE_TOP || i == Projection::PLANE_BOTTOM) {
			CHECK(std::abs(double(relative[i].d)) <= 1.0e-6);
		}
		// Two tangents of the plane.
		const Vector3 u = n.cross(std::abs(n.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
		const Vector3 w = n.cross(u).normalized();
		for (const double along : { 4.0, 20.0, 60.0 }) {
			for (const double delta : { 0.002, -0.002 }) {
				// Camera-relative point (double geometry), then the float values the two paths would see.
				const double q[3] = {
					double(n.x) * double(relative[i].d) + along * double(u.x) + 0.5 * along * double(w.x) + delta * double(n.x),
					double(n.y) * double(relative[i].d) + along * double(u.y) + 0.5 * along * double(w.y) + delta * double(n.y),
					double(n.z) * double(relative[i].d) + along * double(u.z) + 0.5 * along * double(w.z) + delta * double(n.z)
				};
				const bool outside = delta > 0.0;
				// Relative path: float camera-relative point against the relative plane (the shader's test).
				const Vector3 q_f = Vector3(real_t(q[0]), real_t(q[1]), real_t(q[2]));
				const bool relative_outside = n.dot(q_f) - relative[i].d > 0.0f;
				// Absolute path: float absolute point against the absolute plane.
				const double cam_d[3] = { base + 0.3, 0.0, 10.0 };
				const Vector3 p_f = Vector3(real_t(cam_d[0] + q[0]), real_t(cam_d[1] + q[1]), real_t(cam_d[2] + q[2]));
				const bool absolute_outside = absolute[i].normal.dot(p_f) - absolute[i].d > 0.0f;
				relative_wrong += relative_outside != outside ? 1 : 0;
				absolute_wrong += absolute_outside != outside ? 1 : 0;
				++checked;
			}
		}
	}
	CHECK(checked == 36);
	CHECK(relative_wrong == 0);
	CHECK(absolute_wrong > 0); // The pre-C2 absolute frame misclassifies 2 mm borderline points at 100 km.
}

TEST_CASE("[GaussianSplatting][HLOD][C2] tile-binding identity and the sort-reuse key follow the origin revision") {
	// Tile renderer: same buffers, same content generation, new origin revision (a shift reuses the draw storage).
	Vector<TileRenderer::RenderParams> sequence;
	sequence.resize(6);
	sequence.write[0].hlod_relative = true;
	sequence.write[0].hlod_source_generation = 7;
	sequence.write[0].hlod_content_generation = 10;
	sequence.write[0].hlod_origin_revision = 0;
	sequence.write[1] = sequence[0]; // unchanged
	sequence.write[2] = sequence[0];
	sequence.write[2].hlod_origin_revision = 1; // origin shifted
	sequence.write[3] = sequence[2]; // unchanged at the new revision
	sequence.write[4] = sequence[0]; // back to origin revision 0 content: still a different key than revision 1
	sequence.write[4].hlod_origin_revision = 2; // revisions are monotone: a return to the old origin is revision 2
	sequence.write[5] = sequence[4];
	const Vector<uint64_t> trace = TileRenderer::_test_instance_pipeline_binding_generation_trace(sequence);
	if (trace.size() != 6) { FAIL("complete identity trace missing"); return; }
	CHECK(trace[0] == 1);
	CHECK(trace[1] == 1);
	CHECK(trace[2] == 2);
	CHECK(trace[3] == 2);
	CHECK(trace[4] == 3);
	CHECK(trace[5] == 3);

	// Sort reuse: outside the HLOD route the generation is untouched; inside it each revision is its own key.
	const uint64_t gen = 0xABCDEF0123456789ULL;
	CHECK(gs_hlod_gpu::origin_keyed_generation(gen, false, 5) == gen);
	CHECK(gs_hlod_gpu::origin_keyed_generation(gen, false, 0) == gen);
	const uint64_t r0 = gs_hlod_gpu::origin_keyed_generation(gen, true, 0);
	const uint64_t r1 = gs_hlod_gpu::origin_keyed_generation(gen, true, 1);
	const uint64_t r2 = gs_hlod_gpu::origin_keyed_generation(gen, true, 2);
	CHECK(r0 == gs_hlod_gpu::origin_keyed_generation(gen, true, 0));
	CHECK(r0 != r1);
	CHECK(r1 != r2);
	CHECK(r0 != r2);
	CHECK(r0 != gen);
}

} // namespace TestHlodWorldFrame
