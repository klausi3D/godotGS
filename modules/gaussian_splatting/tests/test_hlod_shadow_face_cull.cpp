extern "C" int test_hlod_shadow_face_cull_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../renderer/hlod_shadow_face_cull.h"
#include "../renderer/gaussian_gpu_layout.h"

#include "core/math/random_pcg.h"

#include <cmath>

// Per light view caster lists (renderer/hlod_shadow_face_cull.h). CPU only. The central property: a record the list drops
// from a view has no splat whose quad gs_shadow_caster.glsl would keep, so the view's depth is unchanged. The shader's
// keep/reject decision is mirrored here in float32, line for line (gs_shadow_caster.glsl main, clip to depth tests).
namespace TestHlodShadowFaceCull {
using namespace gs_shadow_face_cull;

struct Splat {
	Vector3 position; // Frame-relative, as the shader's frame_position.
	float cov[6]; // xx, xy, xz, yy, yz, zz
	float opacity = 1.0f;
};

// gs_shadow_caster.glsl: clip -> reject tests -> footprint extent -> view-rect cull -> far/near depth tests.
static bool shader_keeps(const Projection &p_m, const Splat &p_s, const Vector2 &p_size, uint32_t p_mode) {
	const float v[4] = { p_s.position.x, p_s.position.y, p_s.position.z, 1.0f };
	float clip[4];
	for (int r = 0; r < 4; ++r) {
		clip[r] = 0.0f;
		for (int c = 0; c < 4; ++c) { clip[r] += float(p_m.columns[c][r]) * v[c]; }
	}
	const bool perspective = p_mode == 2u;
	if (!(std::fabs(clip[3]) > 1e-6f) || (perspective && clip[3] <= 1e-6f)) { return false; }
	const float ndc[3] = { clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3] };
	const float half_x = 0.5f * p_size.x, half_y = 0.5f * p_size.y;
	float jx[3], jy[3];
	for (int c = 0; c < 3; ++c) {
		jx[c] = float(p_m.columns[c][0]) * half_x;
		jy[c] = float(p_m.columns[c][1]) * half_y;
	}
	if (perspective) {
		for (int c = 0; c < 3; ++c) {
			const float jw = float(p_m.columns[c][3]);
			jx[c] = (jx[c] - ndc[0] * (half_x * jw)) / clip[3];
			jy[c] = (jy[c] - ndc[1] * (half_y * jw)) / clip[3];
		}
	}
	const float *k = p_s.cov;
	auto mul = [k](const float *p_v, float *r_out) {
		r_out[0] = k[0] * p_v[0] + k[1] * p_v[1] + k[2] * p_v[2];
		r_out[1] = k[1] * p_v[0] + k[3] * p_v[1] + k[4] * p_v[2];
		r_out[2] = k[2] * p_v[0] + k[4] * p_v[1] + k[5] * p_v[2];
	};
	float cjx[3], cjy[3];
	mul(jx, cjx);
	mul(jy, cjy);
	const float a_raw = jx[0] * cjx[0] + jx[1] * cjx[1] + jx[2] * cjx[2];
	const float b = jx[0] * cjy[0] + jx[1] * cjy[1] + jx[2] * cjy[2];
	const float c_raw = jy[0] * cjy[0] + jy[1] * cjy[1] + jy[2] * cjy[2];
	const float low_pass = gs_shadow_caster::kLowPassTexels2;
	const gs_shadow_caster::FootprintEigen e = gs_shadow_caster::footprint_eigenvalues(a_raw, b, c_raw, low_pass);
	if (!(e.major > 0.0f) || std::isinf(e.major)) { return false; }
	const float a = a_raw + low_pass, c = c_raw + low_pass;
	float ax = a >= c ? 1.0f : 0.0f, ay = a >= c ? 0.0f : 1.0f;
	if (std::fabs(b) > 1e-12f) {
		ax = a >= c ? e.major - c : b;
		ay = a >= c ? b : e.major - a;
		const float len = std::sqrt(ax * ax + ay * ay);
		ax /= len;
		ay /= len;
	}
	const float threshold = gs_shadow_caster::kAlphaThreshold;
	if (!(p_s.opacity >= threshold)) { return false; }
	const float sigma_extent = std::sqrt(std::fmax(2.0f * std::log(p_s.opacity / threshold), 0.0f));
	const float s0 = std::sqrt(e.major) * sigma_extent, s1 = std::sqrt(e.minor) * sigma_extent;
	const float extent_x = (std::fabs(ax * s0) + std::fabs(-ay * s1)) / half_x;
	const float extent_y = (std::fabs(ay * s0) + std::fabs(ax * s1)) / half_y;
	if (std::fabs(ndc[0]) > 1.0f + extent_x || std::fabs(ndc[1]) > 1.0f + extent_y || ndc[2] < 0.0f) { return false; }
	const bool pancaked = p_mode == 1u && ndc[2] >= 0.9999f;
	return pancaked || ndc[2] <= 1.0f;
}

static void covariance(const Basis &p_rotation, const Vector3 &p_scale, float r_cov[6]) {
	const Basis rs = p_rotation * Basis::from_scale(p_scale);
	const Basis cov = rs * rs.transposed();
	r_cov[0] = cov.rows[0][0];
	r_cov[1] = cov.rows[0][1];
	r_cov[2] = cov.rows[0][2];
	r_cov[3] = cov.rows[1][1];
	r_cov[4] = cov.rows[1][2];
	r_cov[5] = cov.rows[2][2];
}

struct ViewCase {
	Projection clip; // Frame-relative clip, exactly what the caster receives.
	Vector2i size;
	uint32_t mode = 0; // 1 = directional pancake, 2 = perspective.
	Vector3 light;
	const char *name = "";
};

static LocalVector<ViewCase> views(const Vector3 &p_camera) {
	LocalVector<ViewCase> out;
	const Vector3 light(3.0f, 6.0f, -4.0f);
	const float range = 32.0f;
	Transform3D base;
	base.basis = Basis::from_euler(Vector3(-0.3f, 0.4f, 0.1f));
	base.origin = light;
	const Vector3 normals[] = { Vector3(+1, 0, 0), Vector3(-1, 0, 0), Vector3(0, -1, 0), Vector3(0, +1, 0), Vector3(0, 0, +1), Vector3(0, 0, -1) };
	const Vector3 ups[] = { Vector3(0, -1, 0), Vector3(0, -1, 0), Vector3(0, 0, -1), Vector3(0, 0, +1), Vector3(0, -1, 0), Vector3(0, -1, 0) };
	Projection cube;
	cube.set_perspective(90.0f, 1.0f, 0.025f, range);
	for (int face = 0; face < 6; ++face) {
		ViewCase v;
		// Cube faces arrive with flip_y = false (_gaussian_shadow_setup_omni_cube).
		v.clip = gs_shadow_caster::frame_relative_shadow_clip(cube, base * Transform3D().looking_at(normals[face], ups[face]), false, p_camera);
		v.size = Vector2i(512, 512);
		v.mode = 2;
		v.light = light;
		v.name = "omni_face";
		out.push_back(v);
	}
	Projection spot;
	spot.set_perspective(2.0f * 35.0f, 1.0f, 0.025f, 40.0f);
	ViewCase s;
	s.clip = gs_shadow_caster::frame_relative_shadow_clip(spot, base, true, p_camera);
	s.size = Vector2i(1024, 1024);
	s.mode = 2;
	s.light = light;
	s.name = "spot";
	out.push_back(s);
	Projection ortho;
	ortho.set_orthogonal(-20.0f, 20.0f, -20.0f, 20.0f, 0.0f, 60.0f);
	ViewCase d;
	d.clip = gs_shadow_caster::frame_relative_shadow_clip(ortho, base, true, p_camera);
	d.size = Vector2i(2048, 2048);
	d.mode = 1;
	d.light = light;
	d.name = "directional";
	out.push_back(d);
	return out;
}

// Random boxes around the light (many outside every view), each filled with splats whose three-sigma support stays
// inside it. Returns how many dropped boxes still had a kept splat (must be 0) and how many boxes were dropped.
static void sweep(bool p_unsafe, uint32_t &r_violations, uint32_t &r_dropped, uint32_t &r_boxes) {
	r_violations = r_dropped = r_boxes = 0;
	RandomPCG rng(0x5eed1234u);
	for (const Vector3 &camera : { Vector3(), Vector3(-37.5f, 2.0f, 61.0f) }) {
		for (const ViewCase &v : views(camera)) {
			const View view(v.clip, camera, v.size, v.mode == 2, p_unsafe ? 0.0 : kGuardTexels);
			for (int box_index = 0; box_index < 600; ++box_index) {
				const Vector3 center = v.light + Vector3(rng.random(-70.0f, 70.0f), rng.random(-70.0f, 70.0f), rng.random(-70.0f, 70.0f));
				const Vector3 half(rng.random(0.2f, 6.0f), rng.random(0.2f, 6.0f), rng.random(0.2f, 6.0f));
				const AABB box(center - half, half * 2.0f);
				const AABB tested = p_unsafe ? AABB(box.get_center(), Vector3()) : box;
				const bool dropped = view.box_outside(tested);
				++r_boxes;
				r_dropped += dropped ? 1u : 0u;
				if (!dropped) { continue; }
				bool kept = false;
				for (int i = 0; i < 48 && !kept; ++i) {
					const float max_sigma = std::fmin(half.x, std::fmin(half.y, half.z)) / 3.0f;
					// Every third splat is as large as the box allows: the worst case for the support bound.
					const float sigma_hi = (i % 3 == 0) ? max_sigma : rng.random(0.001f, max_sigma);
					const Vector3 scale(sigma_hi, rng.random(0.0005f, sigma_hi), rng.random(0.0005f, sigma_hi));
					const Basis rotation(Quaternion(Vector3(rng.randf() - 0.5f, rng.randf() - 0.5f, rng.randf() - 0.5f).normalized(),
							rng.random(0.0f, 6.2831853f)));
					const float reach = 3.0f * std::fmax(scale.x, std::fmax(scale.y, scale.z));
					Vector3 p;
					for (int a = 0; a < 3; ++a) {
						const float lo = box.position[a] + reach, hi = box.position[a] + box.size[a] - reach;
						// Hug the faces too: corners and edges are where an unsafe cull leaks.
						p[a] = (i % 2 == 0) ? (rng.randf() < 0.5f ? lo : hi) : rng.random(lo, std::fmax(lo, hi));
					}
					Splat s;
					s.position = p - camera;
					covariance(rotation, scale, s.cov);
					s.opacity = rng.random(0.5f, 1.0f);
					kept = shader_keeps(v.clip, s, Vector2(v.size), v.mode);
				}
				r_violations += kept ? 1u : 0u;
			}
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters][ViewLists] no dropped record has a splat the caster shader keeps") {
	uint32_t violations = 0, dropped = 0, boxes = 0;
	sweep(false, violations, dropped, boxes);
	CHECK_MESSAGE(violations == 0, "dropped boxes with a kept splat: ", violations, " of ", dropped);
	// The lists must actually shrink views; a culler that drops nothing would pass the line above vacuously.
	CHECK_MESSAGE(dropped * 4 > boxes, "dropped ", dropped, " of ", boxes);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters][ViewLists] the sweep detects an unsafe centre-only culler") {
	// Negative control for the case above: culling by the box centre with no guard band drops records that still reach
	// the view, and the same corpus and shader mirror must see it.
	uint32_t violations = 0, dropped = 0, boxes = 0;
	sweep(true, violations, dropped, boxes);
	CHECK_MESSAGE(violations > 0, "the corpus cannot detect an unsafe cull (", dropped, " dropped)");
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters][ViewLists] boxes behind, beyond far and through the light") {
	const Vector3 camera(10.0f, 0.0f, 0.0f);
	Projection cube;
	cube.set_perspective(90.0f, 1.0f, 0.025f, 32.0f);
	Transform3D light; // At the origin, face looking down -Z.
	const View view(gs_shadow_caster::frame_relative_shadow_clip(cube, light, false, camera), camera, Vector2i(512, 512), true);
	CHECK(view.box_outside(AABB(Vector3(-1, -1, 5), Vector3(2, 2, 2)))); // Behind the face.
	CHECK(view.box_outside(AABB(Vector3(-1, -1, -60), Vector3(2, 2, 2)))); // Beyond the far plane.
	CHECK(view.box_outside(AABB(Vector3(30, -1, -12), Vector3(2, 2, 2)))); // Off the side.
	CHECK_FALSE(view.box_outside(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)))); // Contains the light.
	CHECK_FALSE(view.box_outside(AABB(Vector3(-1, -1, -12), Vector3(2, 2, 2)))); // In front.
	CHECK_FALSE(view.box_outside(AABB(Vector3(-1, -1, -33), Vector3(2, 2, 2)))); // Straddles far.
	CHECK_FALSE(View().box_outside(AABB(Vector3(-1, -1, 5), Vector3(2, 2, 2)))); // No view: never cull.
	AABB nan_box(Vector3(NAN, 0, 0), Vector3(1, 1, 1));
	CHECK_FALSE(view.box_outside(nan_box));
	// A directional cascade never culls in front of its near plane: those casters are pancaked.
	Projection ortho;
	ortho.set_orthogonal(-5.0f, 5.0f, -5.0f, 5.0f, 1.0f, 20.0f);
	const View cascade(gs_shadow_caster::frame_relative_shadow_clip(ortho, light, true, camera), camera, Vector2i(1024, 1024), false);
	CHECK_FALSE(cascade.box_outside(AABB(Vector3(-1, -1, 40), Vector3(2, 2, 2))));
	CHECK(cascade.box_outside(AABB(Vector3(-1, -1, -60), Vector3(2, 2, 2))));
}

static gs_shadow_caster::FrameRecord record_of(uint32_t p_draws) {
	gs_shadow_caster::FrameRecord r;
	r.valid = true;
	r.draw_count = p_draws;
	r.bundle_builds = 7;
	r.draw_list_upload = 11;
	r.max_chunk_splats = 64;
	return r;
}

static InstanceDataGPU identity_row() {
	InstanceDataGPU row = {};
	row.rotation[3] = 1.0f;
	row.translation_scale[3] = 1.0f;
	row.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO;
	return row;
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters][ViewLists] lists keep unbounded records and skip non-casting rows like the shader") {
	// Draws: 0 world (bounded, far behind), 1 world (bounded, in view), 2 object (unknown support), 3 object of a
	// non-casting row, 4 object of a row past the uploaded flags and rows (the shader decides).
	gs_shadow_refresh::PublishedContent content;
	content.gpu_draws.resize(5);
	content.geometry.resize(5);
	const uint32_t draw_rows[5] = { 0, 0, 1, 2, 9 };
	for (uint32_t i = 0; i < 5; ++i) {
		content.gpu_draws[i] = DrawnNodeGPU{};
		content.gpu_draws[i].instance_id = draw_rows[i];
	}
	content.geometry[0].bounds = AABB(Vector3(-1, -1, 50), Vector3(2, 2, 2));
	content.geometry[0].support_kind = SUPPORT_WORLD_NODE_AABB;
	content.geometry[1].bounds = AABB(Vector3(-1, -1, -10), Vector3(2, 2, 2));
	content.geometry[1].support_kind = SUPPORT_WORLD_NODE_AABB;
	content.geometry[2].bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)); // support_kind unknown
	content.geometry[3].bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	content.geometry[3].support_kind = SUPPORT_LOCAL_SPHERE;
	content.geometry[3].support_radius = 1.0f;
	content.geometry[4] = content.geometry[3];
	LocalVector<InstanceDataGPU> rows;
	LocalVector<real_t> margins;
	for (int r = 0; r < 3; ++r) {
		rows.push_back(identity_row());
		margins.push_back(0.0f);
	}
	rows[1].translation_scale[2] = 80.0f; // Object row 1 is far behind the face, but its support is unknown.
	rows[2].translation_scale[2] = -10.0f;
	LocalVector<uint32_t> casts;
	casts.push_back(1);
	casts.push_back(1);
	casts.push_back(0); // Row 2 does not cast.
	Snapshot snapshot;
	snapshot.build(record_of(5), content, rows, margins, 42);
	if (!snapshot.valid) {
		FAIL("snapshot refused a consistent list");
		return;
	}
	CHECK(snapshot.known[0] == 1);
	CHECK(snapshot.known[2] == 0);
	CHECK(snapshot.known[4] == 0); // Row 9 has no row data.
	Projection cube;
	cube.set_perspective(90.0f, 1.0f, 0.025f, 32.0f);
	const View view(gs_shadow_caster::frame_relative_shadow_clip(cube, Transform3D(), false, Vector3()), Vector3(), Vector2i(512, 512), true);
	LocalVector<uint32_t> list;
	ListStats stats;
	build_view_list(snapshot, casts, view, list, stats);
	if (list.size() != 3) {
		FAIL("expected draws 1, 2, 4; got ", list.size());
		return;
	}
	CHECK(list[0] == 1);
	CHECK(list[1] == 2);
	CHECK(list[2] == 4);
	CHECK(stats.culled_outside == 1);
	CHECK(stats.culled_noncasting == 1);
	CHECK(stats.unbounded == 2);
	CHECK_FALSE(stats.placeholder);
	// A world row that is not identity-placed keeps every world record.
	rows[0].ids[1] = 0;
	snapshot.build(record_of(5), content, rows, margins, 42);
	build_view_list(snapshot, casts, view, list, stats);
	CHECK(stats.culled_outside == 0);
	CHECK(list.size() == 4);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters][ViewLists] an empty view keeps one record proven to emit nothing") {
	gs_shadow_refresh::PublishedContent content;
	content.gpu_draws.resize(2);
	content.geometry.resize(2);
	for (uint32_t i = 0; i < 2; ++i) {
		content.gpu_draws[i] = DrawnNodeGPU{};
		content.geometry[i].bounds = AABB(Vector3(-1, -1, 40.0f + 5.0f * i), Vector3(2, 2, 2));
		content.geometry[i].support_kind = SUPPORT_WORLD_NODE_AABB;
	}
	LocalVector<InstanceDataGPU> rows;
	rows.push_back(identity_row());
	LocalVector<real_t> margins;
	margins.push_back(0.0f);
	LocalVector<uint32_t> casts;
	casts.push_back(1);
	Snapshot snapshot;
	snapshot.build(record_of(2), content, rows, margins, 1);
	Projection cube;
	cube.set_perspective(90.0f, 1.0f, 0.025f, 32.0f);
	const View view(gs_shadow_caster::frame_relative_shadow_clip(cube, Transform3D(), false, Vector3()), Vector3(), Vector2i(512, 512), true);
	LocalVector<uint32_t> list;
	ListStats stats;
	build_view_list(snapshot, casts, view, list, stats);
	if (list.size() != 1) {
		FAIL("expected one placeholder record, got ", list.size());
		return;
	}
	CHECK(list[0] == 0);
	CHECK(stats.placeholder);
	CHECK(stats.culled_outside == 2);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters][ViewLists] a snapshot serves only its exact list and RS frame") {
	gs_shadow_refresh::PublishedContent content;
	content.gpu_draws.resize(1);
	content.geometry.resize(1);
	content.gpu_draws[0] = DrawnNodeGPU{};
	LocalVector<InstanceDataGPU> rows;
	rows.push_back(identity_row());
	LocalVector<real_t> margins;
	margins.push_back(0.0f);
	const gs_shadow_caster::FrameRecord record = record_of(1);
	Snapshot snapshot;
	snapshot.build(record, content, rows, margins, 5);
	CHECK(snapshot.matches(record, 5));
	CHECK_FALSE(snapshot.matches(record, 6)); // Next RS frame: rows may have moved.
	gs_shadow_caster::FrameRecord changed = record;
	changed.draw_list_upload++;
	CHECK_FALSE(snapshot.matches(changed, 5));
	changed = record;
	changed.bundle_builds++;
	CHECK_FALSE(snapshot.matches(changed, 5));
	changed = record;
	changed.draw_count = 2;
	CHECK_FALSE(snapshot.matches(changed, 5));
	// Inconsistent inputs never produce a snapshot.
	snapshot.build(record_of(2), content, rows, margins, 5);
	CHECK_FALSE(snapshot.valid);
	LocalVector<real_t> short_margins;
	snapshot.build(record, content, rows, short_margins, 5);
	CHECK_FALSE(snapshot.valid);
	snapshot.build(gs_shadow_caster::FrameRecord(), content, rows, margins, 5);
	CHECK_FALSE(snapshot.matches(record, 5));
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters][ViewLists] object support encloses every splat's three-sigma reach") {
	PackedGaussian splats[3] = {};
	splats[0].position[0] = 1.0f;
	splats[0].scale[0] = splats[0].scale[1] = splats[0].scale[2] = 0.1f;
	splats[1].position[1] = -2.0f;
	splats[1].scale[2] = 0.5f; // Long axis dominates.
	splats[2].position[2] = 0.5f;
	real_t radius = -1;
	if (!local_support_radius(splats, 3, Vector3(), radius)) {
		FAIL("finite payload refused");
		return;
	}
	CHECK(radius >= 2.0f + 1.5f);
	CHECK(radius < 3.6f);
	splats[2].scale[0] = INFINITY;
	CHECK_FALSE(local_support_radius(splats, 3, Vector3(), radius));
	CHECK_FALSE(local_support_radius<PackedGaussian>(nullptr, 0, Vector3(), radius));
	// Placement: both flag branches of the shader (scaled or not) stay inside the box; margins grow it.
	gs_shadow_refresh::Geometry geometry;
	geometry.bounds = AABB(Vector3(0.5f, 0.5f, 0.5f), Vector3(1, 1, 1)); // Centre (1, 1, 1).
	geometry.support_kind = SUPPORT_LOCAL_SPHERE;
	geometry.support_radius = 0.25f;
	InstanceDataGPU row = identity_row();
	row.ids[1] = 0;
	row.translation_scale[0] = 100.0f;
	row.translation_scale[3] = -3.0f; // The shader uses |w|.
	AABB box;
	if (!record_support(geometry, 1, row, 0.5f, box)) {
		FAIL("bounded object refused");
		return;
	}
	const Vector3 far_point = Vector3(100, 0, 0) + 3.0f * (Vector3(1, 1, 1) + Vector3(0.25f, 0.25f, 0.25f).normalized() * 0.25f);
	CHECK(box.has_point(far_point));
	CHECK(box.has_point(Vector3(100, 0, 0) - Vector3(1, 1, 1) * (3.0f * (std::sqrt(3.0f) + 0.25f) + 0.49f)));
	geometry.support_radius = -1;
	CHECK_FALSE(record_support(geometry, 1, row, 0.5f, box));
	geometry.support_radius = 0.25f;
	CHECK_FALSE(record_support(geometry, 1, row, NAN, box));
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters][ViewLists] frame stats count view-list and whole-list passes per frame") {
	gs_shadow_caster::FrameStats stats;
	gs_shadow_caster::PassPlan cast;
	cast.casters = 10;
	cast.instance_slots = 64;
	gs_shadow_caster::PassPlan refused;
	refused.refusal = gs_shadow_caster::Refusal::BUNDLE_CHANGED;
	stats.record(3, cast);
	stats.record_list(cast, true, 4, 10);
	stats.record(3, cast);
	stats.record_list(cast, false, 0, 0);
	stats.record(3, refused);
	stats.record_list(refused, true, 1, 10);
	CHECK(stats.view_list_passes == 1);
	CHECK(stats.whole_list_passes == 1);
	CHECK(stats.view_list_records == 4);
	CHECK(stats.view_list_available == 10);
	stats.record(4, cast);
	CHECK(stats.view_list_passes == 0);
	CHECK(stats.whole_list_passes == 0);
	CHECK(stats.view_list_records == 0);
}

} // namespace TestHlodShadowFaceCull
