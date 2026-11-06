extern "C" int test_hlod_shadow_casters_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../renderer/hlod_shadow_caster_plan.h"
#include "../renderer/hlod_world_instances.h"

#include <cmath>

// Splat shadow casters on the HLOD route (#1095, #1163; renderer/hlod_shadow_caster_plan.h). CPU only: the caster list a
// light pass draws (the last active main view's draw list), its refusals, the caster budget (exactly the view's admitted
// set), the per-frame accounting, and the clip transform (Godot's directional shadow convention, camera-relative input).
namespace TestHlodShadowCasters {
using namespace gs_shadow_caster;

static FrameRecord active_record() {
	FrameRecord r;
	r.valid = true;
	r.draw_count = 12;
	r.world_records = 10;
	r.instance_records = 2;
	r.max_chunk_splats = 4096;
	r.world_splats = 25600;
	r.instance_splats = 3000;
	r.camera = Vector3(1.0f, 2.0f, 3.0f);
	return r;
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] a directional pass casts exactly the main view's draw list") {
	const FrameRecord record = active_record();
	const PassPlan plan = plan_pass(record, true, true);
	CHECK(plan.draws());
	CHECK(plan.refusal == Refusal::NONE);
	CHECK(plan.casters == 25600 + 3000);
	CHECK(plan.instance_slots == 12u * 4096u);
	CHECK(String(refusal_reason(plan.refusal)) == "cast");
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] every pass that cannot cast refuses with its own reason and no casters") {
	FrameRecord record = active_record();
	struct Case {
		FrameRecord record;
		bool ortho;
		bool current;
		Refusal expected;
		const char *reason;
	};
	FrameRecord invalid = record;
	invalid.valid = false;
	FrameRecord quantized = record;
	quantized.quantized = true;
	FrameRecord empty = record;
	empty.draw_count = 0;
	FrameRecord no_stride = record;
	no_stride.max_chunk_splats = 0;
	FrameRecord no_splats = record;
	no_splats.world_splats = no_splats.instance_splats = 0;
	FrameRecord overflow = record;
	overflow.draw_count = 1u << 20;
	overflow.max_chunk_splats = 1u << 12; // 2^32 slots > kMaxInstanceSlots.
	const Case cases[] = {
		{ record, false, true, Refusal::NOT_DIRECTIONAL, "shadow_caster_light_type_not_directional" },
		{ invalid, true, true, Refusal::NO_ACTIVE_HLOD_VIEW, "shadow_caster_no_active_hlod_view" },
		{ record, true, false, Refusal::BUNDLE_CHANGED, "shadow_caster_bundle_changed" },
		{ quantized, true, true, Refusal::QUANTIZED_PAYLOAD, "shadow_caster_quantized_payload_unsupported" },
		{ empty, true, true, Refusal::EMPTY_DRAW_LIST, "shadow_caster_empty_draw_list" },
		{ no_stride, true, true, Refusal::EMPTY_DRAW_LIST, "shadow_caster_empty_draw_list" },
		{ no_splats, true, true, Refusal::EMPTY_DRAW_LIST, "shadow_caster_empty_draw_list" },
		{ overflow, true, true, Refusal::INSTANCE_SLOT_OVERFLOW, "shadow_caster_instance_slot_overflow" },
	};
	for (const Case &c : cases) {
		const PassPlan plan = plan_pass(c.record, c.ortho, c.current);
		CHECK_MESSAGE(plan.refusal == c.expected, c.reason);
		CHECK(!plan.draws());
		CHECK(plan.casters == 0);
		CHECK(plan.instance_slots == 0);
		CHECK(String(refusal_reason(plan.refusal)) == c.reason);
	}
	// A non-directional pass refuses even when nothing else is ready: the light type is named first.
	CHECK(plan_pass(invalid, false, false).refusal == Refusal::NOT_DIRECTIONAL);
	// The largest list that fits is drawn whole, never truncated.
	FrameRecord edge = record;
	edge.draw_count = 1u << 19;
	edge.max_chunk_splats = 1u << 12; // exactly kMaxInstanceSlots
	const PassPlan edge_plan = plan_pass(edge, true, true);
	CHECK(edge_plan.draws());
	CHECK(uint64_t(edge_plan.instance_slots) == kMaxInstanceSlots);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] the caster budget is the view's admitted set: refused objects cast nothing") {
	using namespace gs_hlod_bundle;
	InstanceRegion region;
	for (uint32_t i = 0; i < 4; ++i) {
		InstanceAsset asset;
		asset.asset_id = 100 + i;
		asset.splat_count = 1500 + i; // Distinct, so a wrong asset would show.
		asset.chunk_count = 1;
		region.assets.push_back(asset);
	}
	ObjectDemand demands[4];
	for (uint32_t i = 0; i < 4; ++i) {
		demands[i].instance_key = 10 + i;
		demands[i].row = 1 + i;
		demands[i].asset_index = i;
		demands[i].splats = region.assets[i].splat_count;
		demands[i].chunks = 1;
		demands[i].records = 1.0;
		demands[i].importance = double(4 - i); // Row 1 most important.
	}
	// Room for exactly two objects.
	ObjectShare share;
	share.splats = 1500 + 1501 + 10;
	share.nodes = 100;
	share.records = 100.0;
	Admission admission;
	admit_objects(demands, 4, share, admission);
	if (admission.admitted.size() != 2 || admission.refused.size() != 2) {
		FAIL("expected 2 admitted and 2 refused objects, got ", admission.admitted.size(), " / ", admission.refused.size());
		return;
	}
	// Rows as uploaded: 0 = world, 1..4 = the objects; every node casts.
	LocalVector<uint64_t> row_keys;
	row_keys.push_back(0);
	LocalVector<uint64_t> caster_keys;
	for (uint32_t i = 0; i < 4; ++i) {
		row_keys.push_back(10 + i);
		caster_keys.push_back(10 + i);
	}
	LocalVector<uint32_t> row_casts;
	build_row_casts(row_keys, true, caster_keys, row_casts);
	if (row_casts.size() != 5) {
		FAIL("expected 5 row flags, got ", row_casts.size());
		return;
	}
	for (uint32_t r = 0; r < 5; ++r) {
		CHECK(row_casts[r] == 1u);
	}
	const uint64_t casters = admitted_instance_splats(region, demands, 4, admission, row_casts);
	CHECK(casters == 1500 + 1501);
	CHECK(casters == admission.splats); // The list the color view draws, splat for splat.
	CHECK(casters <= share.splats); // Never more than the object share of the view cap.
	// A node with cast_shadow off (key 11, row 2) is drawn but casts nothing; the world row casts with the world.
	caster_keys.erase(11);
	build_row_casts(row_keys, true, caster_keys, row_casts);
	CHECK(row_casts[0] == 1u);
	CHECK(row_casts[2] == 0u);
	CHECK(admitted_instance_splats(region, demands, 4, admission, row_casts) == 1500);
	// No caster keys at all: only the (casting) world row casts.
	LocalVector<uint64_t> no_keys;
	build_row_casts(row_keys, true, no_keys, row_casts);
	CHECK(row_casts[0] == 1u);
	CHECK(admitted_instance_splats(region, demands, 4, admission, row_casts) == 0);
	// No objects admitted: no instance casters; an out-of-range demand index or row is ignored, never read.
	build_row_casts(row_keys, true, caster_keys, row_casts);
	Admission none;
	CHECK(admitted_instance_splats(region, demands, 4, none, row_casts) == 0);
	Admission bogus;
	bogus.admitted.push_back(7);
	CHECK(admitted_instance_splats(region, demands, 4, bogus, row_casts) == 0);
	LocalVector<uint32_t> short_casts;
	short_casts.push_back(1);
	CHECK(admitted_instance_splats(region, demands, 4, admission, short_casts) == 0);
	// The pass then casts world + admitted objects.
	FrameRecord record = active_record();
	record.instance_splats = casters;
	CHECK(plan_pass(record, true, true).casters == record.world_splats + 1500 + 1501);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] the world row follows the world's own cast_shadow, independently of node casters") {
	// Godot gives the shared renderer a shadow pass when ANY of its Gaussian instances casts (renderer_scene_render_rd.cpp),
	// so a casting node must never make a world with cast_shadow off cast (Codex P2 on #1316).
	LocalVector<uint64_t> row_keys;
	row_keys.push_back(0); // Row 0: the world.
	row_keys.push_back(11);
	row_keys.push_back(12);
	LocalVector<uint64_t> caster_keys;
	caster_keys.push_back(11); // Node 11 casts, node 12 does not.
	LocalVector<uint32_t> row_casts;
	build_row_casts(row_keys, false, caster_keys, row_casts);
	if (row_casts.size() != 3) {
		FAIL("expected 3 row flags, got ", row_casts.size());
		return;
	}
	CHECK(row_casts[0] == 0u); // World off, although a node casts.
	CHECK(row_casts[1] == 1u);
	CHECK(row_casts[2] == 0u);
	build_row_casts(row_keys, true, caster_keys, row_casts);
	CHECK(row_casts[0] == 1u);
	CHECK(row_casts[1] == 1u);
	CHECK(row_casts[2] == 0u);
	// A node keyed 0 never stands in for the world: row 0 is decided by the world flag alone.
	LocalVector<uint64_t> zero_key;
	zero_key.push_back(0);
	build_row_casts(row_keys, false, zero_key, row_casts);
	CHECK(row_casts[0] == 0u);

	// The record's caster accounting: a world that does not cast contributes no splats, its records stay in the list (the
	// shader masks the row), and with no casting object the pass refuses as empty instead of drawing.
	FrameRecord world_off;
	world_off.instance_records = 2;
	world_off.instance_splats = 1500;
	world_off.max_chunk_splats = 4096;
	build_row_casts(row_keys, false, caster_keys, world_off.row_casts);
	complete_record(world_off, 10, 25600);
	CHECK(world_off.valid);
	CHECK(world_off.world_records == 10);
	CHECK(world_off.draw_count == 12);
	CHECK(world_off.world_splats == 0);
	CHECK(plan_pass(world_off, true, true).casters == 1500);
	FrameRecord world_on = world_off;
	build_row_casts(row_keys, true, caster_keys, world_on.row_casts);
	complete_record(world_on, 10, 25600);
	CHECK(world_on.world_splats == 25600);
	CHECK(plan_pass(world_on, true, true).casters == 25600 + 1500);
	FrameRecord nothing = world_off;
	nothing.instance_splats = 0;
	complete_record(nothing, 10, 25600);
	CHECK(plan_pass(nothing, true, true).refusal == Refusal::EMPTY_DRAW_LIST);
	// No rows at all (no flags): the world never casts by default.
	FrameRecord no_rows;
	no_rows.max_chunk_splats = 4096;
	complete_record(no_rows, 10, 25600);
	CHECK(no_rows.world_splats == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] an idle streamed world keeps its drawn objects casting: an instance-only record") {
	// A paged world whose cut is loading or not admitted still draws the admitted objects (ADR 4.4, idle_hlod_streaming_bundle
	// with instances): the uploaded list is the instance records alone, and that list casts (Codex P2 on #1316).
	LocalVector<uint64_t> row_keys;
	row_keys.push_back(0);
	row_keys.push_back(11);
	LocalVector<uint64_t> caster_keys;
	caster_keys.push_back(11);
	FrameRecord idle;
	idle.instance_records = 3;
	idle.instance_splats = 1500;
	idle.max_chunk_splats = 4096;
	build_row_casts(row_keys, true, caster_keys, idle.row_casts);
	complete_record(idle, 0, 0);
	CHECK(idle.valid);
	CHECK(idle.world_records == 0);
	CHECK(idle.draw_count == 3); // Exactly the uploaded instance records.
	CHECK(idle.world_splats == 0);
	const PassPlan plan = plan_pass(idle, true, true);
	CHECK(plan.draws());
	CHECK(plan.casters == 1500);
	CHECK(plan.instance_slots == 3u * 4096u);
	// The objects' cast_shadow off: the instance-only list has nothing to cast and refuses, it never draws empty.
	FrameRecord idle_off = idle;
	idle_off.instance_splats = 0;
	complete_record(idle_off, 0, 0);
	CHECK(plan_pass(idle_off, true, true).refusal == Refusal::EMPTY_DRAW_LIST);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] per-frame accounting sums the cascades of one frame and resets on the next") {
	FrameStats stats;
	const PassPlan cast = plan_pass(active_record(), true, true);
	const PassPlan spot = plan_pass(active_record(), false, true);
	stats.record(41, cast);
	stats.record(41, cast);
	stats.record(41, cast);
	stats.record(41, cast); // PSSM4
	stats.record(41, spot);
	CHECK(stats.frame == 41);
	CHECK(stats.passes_cast == 4);
	CHECK(stats.passes_refused == 1);
	CHECK(stats.casters == 4 * cast.casters);
	CHECK(stats.instance_slots == 4ull * cast.instance_slots);
	CHECK(stats.last_refusal == Refusal::NOT_DIRECTIONAL);
	stats.record(42, cast);
	CHECK(stats.frame == 42);
	CHECK(stats.passes_cast == 1);
	CHECK(stats.passes_refused == 0);
	CHECK(stats.casters == cast.casters);
	CHECK(stats.last_refusal == Refusal::NONE);
	CHECK(stats.total_passes_cast == 5);
	CHECK(stats.total_passes_refused == 1);
	// A frame with only refusals casts nothing.
	stats.record(43, plan_pass(FrameRecord(), true, true));
	CHECK(stats.casters == 0);
	CHECK(stats.passes_cast == 0);
	CHECK(stats.last_refusal == Refusal::NO_ACTIVE_HLOD_VIEW);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] the caster writes every cascade texel where Godot's receivers sample it") {
	// A cascade like Godot builds it: orthographic, light looking down at a slant, origin near the camera.
	Projection ortho;
	ortho.set_orthogonal(-8.0f, 8.0f, -8.0f, 8.0f, 0.0f, 60.0f);
	Transform3D light;
	light.basis = Basis::from_euler(Vector3(Math::deg_to_rad(-60.0f), Math::deg_to_rad(20.0f), 0.0f));
	light.origin = Vector3(3.0f, 30.0f, 5.0f);
	const Vector3 forward = -light.basis.get_column(2);
	const Vector3 right = light.basis.get_column(0);
	const Vector3 up = light.basis.get_column(1);
	// The four PSSM4 rects of a directional atlas (normalised), as _render_shadow_pass lays them out.
	const Rect2 rects[] = { Rect2(0.0f, 0.0f, 0.5f, 0.5f), Rect2(0.5f, 0.0f, 0.5f, 0.5f), Rect2(0.0f, 0.5f, 0.5f, 0.5f),
		Rect2(0.5f, 0.5f, 0.5f, 0.5f) };
	// Points off the cascade's centre line in both light-space axes: a mirrored axis cannot coincide by accident.
	const Vector3 points[] = { light.origin + forward * 10.0f + right * 5.0f + up * 6.0f,
		light.origin + forward * 25.0f - right * 7.0f + up * 2.5f, light.origin + forward * 50.0f + right * 1.0f - up * 6.5f };
	// The hook's flip_y for a directional pass is true, exactly what _render_shadow_pass hands Godot's mesh casters.
	const Projection clip = godot_shadow_clip(ortho, light, true);
	const Projection mirrored = godot_shadow_clip(ortho, light, false); // The pre-review convention: must NOT match.
	int mirrored_mismatches = 0;
	for (const Rect2 &rect : rects) {
		for (const Vector3 &p : points) {
			const Vector3 receiver = godot_directional_receiver_sample(ortho, light, rect, p);
			const Vector3 caster = caster_atlas_sample(clip, rect, p);
			CHECK(std::fabs(caster.x - receiver.x) <= 1e-5f);
			CHECK(std::fabs(caster.y - receiver.y) <= 1e-5f);
			CHECK(std::fabs(caster.z - receiver.z) <= 1e-5f);
			// The caster stays inside its own rect: no cascade writes into another's.
			CHECK(caster.x >= rect.position.x - 1e-6f);
			CHECK(caster.x <= rect.position.x + rect.size.x + 1e-6f);
			CHECK(caster.y >= rect.position.y - 1e-6f);
			CHECK(caster.y <= rect.position.y + rect.size.y + 1e-6f);
			const Vector3 wrong = caster_atlas_sample(mirrored, rect, p);
			mirrored_mismatches += std::fabs(wrong.y - receiver.y) > 1e-3f ? 1 : 0;
		}
	}
	CHECK(mirrored_mismatches == 4 * 3);
	// Reverse-Z: the near plane maps to depth 1, the far plane to 0.
	CHECK(clip.xform(light.origin).z == doctest::Approx(1.0f).epsilon(1e-4));
	CHECK(clip.xform(light.origin + forward * 60.0f).z == doctest::Approx(0.0f).epsilon(1e-4));
	// A camera-relative position p (world = p + C) lands where the world position does, near and far from the origin.
	const Vector3 cameras[] = { Vector3(0, 0, 0), Vector3(1.5f, 2.0f, -3.0f), Vector3(1000.0f, 40.0f, -2000.0f) };
	for (const Vector3 &camera : cameras) {
		Transform3D moved = light;
		moved.origin += camera; // Cascades follow the camera.
		const Projection relative_clip = frame_relative_shadow_clip(ortho, moved, true, camera);
		const Projection moved_world_clip = godot_shadow_clip(ortho, moved, true);
		const Vector3 samples[] = { Vector3(0, 0, 0), Vector3(2.0f, -1.0f, 4.0f), Vector3(-5.0f, 0.5f, -7.0f) };
		for (const Vector3 &p : samples) {
			const Vector3 a = relative_clip.xform(p);
			const Vector3 b = moved_world_clip.xform(p + camera);
			const float tolerance = camera.length() > 100.0f ? 2e-3f : 1e-5f;
			CHECK(std::fabs(a.x - b.x) <= tolerance);
			CHECK(std::fabs(a.y - b.y) <= tolerance);
			CHECK(std::fabs(a.z - b.z) <= tolerance);
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] casters in front of a cascade's near plane are pancaked, never clipped") {
	CHECK(pancake_depth(1.5f) == kPancakeDepth);
	CHECK(pancake_depth(0.9999f) == kPancakeDepth);
	CHECK(pancake_depth(0.5f) == 0.5f);
	CHECK(pancake_depth(0.0f) == 0.0f);
	// The occluding support at the alpha threshold: opacity * exp(-r^2 / 2) = threshold at r = sqrt(2 ln(o / t)).
	const float opacity = 0.97f;
	const float r = std::sqrt(2.0f * std::log(opacity / kAlphaThreshold));
	CHECK(opacity * std::exp(-0.5f * r * r) == doctest::Approx(kAlphaThreshold).epsilon(1e-5));
	CHECK(kAlphaThreshold > 0.0f);
	CHECK(kAlphaThreshold < 1.0f);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] an elongated splat keeps its low-pass minor axis in float32") {
	// The review case: scales (10, 1e-5) at 512 texels per unit, axis-aligned. Exact minor eigenvalue = c + w ~ 0.300026.
	const float texels = 512.0f;
	const float a = 100.0f * texels * texels; // 26214400
	const float c = 1e-10f * texels * texels;
	const FootprintEigen e = footprint_eigenvalues(a, 0.0f, c, kLowPassTexels2);
	CHECK(e.major == doctest::Approx(a + kLowPassTexels2).epsilon(1e-6));
	CHECK(e.minor == doctest::Approx(c + kLowPassTexels2).epsilon(1e-4));
	// The previous mid - disc form loses it: discriminates the fix.
	const float aw = a + kLowPassTexels2;
	const float cw = c + kLowPassTexels2;
	const float mid = 0.5f * (aw + cw);
	const float disc = std::sqrt(std::fmax(mid * mid - aw * cw, 0.0f));
	CHECK(std::fmax(mid - disc, 1e-8f) < 0.1f * kLowPassTexels2);

	// Rotated by 30 degrees: the same Gaussian, now with b != 0. The minor axis still never drops below the low-pass.
	const float major_raw = a;
	const float minor_raw = c;
	const float cs = std::cos(0.5235988f);
	const float sn = std::sin(0.5235988f);
	const float ra = major_raw * cs * cs + minor_raw * sn * sn;
	const float rb = (major_raw - minor_raw) * cs * sn;
	const float rc = major_raw * sn * sn + minor_raw * cs * cs;
	const FootprintEigen r = footprint_eigenvalues(ra, rb, rc, kLowPassTexels2);
	CHECK(r.major == doctest::Approx(a + kLowPassTexels2).epsilon(1e-5));
	// Rotated, a * c - b * b itself cancels in float32 (the input is ill-conditioned), so only the floor is exact.
	CHECK(r.minor >= kLowPassTexels2);
	CHECK(std::isfinite(r.minor));
	CHECK(r.minor <= r.major);

	// A round splat: both eigenvalues are the same closed form.
	const FootprintEigen round = footprint_eigenvalues(4.0f, 0.0f, 4.0f, kLowPassTexels2);
	CHECK(round.major == doctest::Approx(4.0f + kLowPassTexels2));
	CHECK(round.minor == doctest::Approx(4.0f + kLowPassTexels2));
	// A general well-conditioned case matches the textbook roots.
	const FootprintEigen g = footprint_eigenvalues(5.0f, 2.0f, 3.0f, 0.0f);
	const float gm = 4.0f;
	const float gd = std::sqrt(1.0f + 4.0f);
	CHECK(g.major == doctest::Approx(gm + gd));
	CHECK(g.minor == doctest::Approx(gm - gd));
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] the depth plane keeps its slope inside the ellipse; only the cap clamps") {
	// The review case: projected covariance ~ identity, gradient g = (k, k), quad corners at (+-e, +-e) in texels for
	// sigma_extent e. The Gaussian's depth extent caps |dz| at ~sqrt(2) k e, below the same-sign corners' 2 k e.
	const float k = 1e-4f;
	const float e = std::sqrt(2.0f * std::log(0.97f / kAlphaThreshold));
	const float cap = std::sqrt(2.0f) * k * e;
	const float center = 0.5f;
	auto plane = [&](float p_u, float p_v) { return k * p_u + k * p_v; };
	// Fragments inside the alpha ellipse (|u| <= e): the per-fragment depth is the exact plane.
	const float inside[][2] = { { 0.5f * e, 0.5f * e }, { 0.7f * e, 0.0f }, { -0.3f * e, 0.6f * e }, { 0.6f * e, 0.6f * e } };
	for (const auto &u : inside) {
		const float d = caster_fragment_depth(center, plane(u[0], u[1]), cap, false);
		CHECK(d == doctest::Approx(center + plane(u[0], u[1])).epsilon(1e-6));
	}
	// The old corner clamp: corners (+e, +e) and (-e, -e) clamp to +-cap, the diagonal of both quad triangles runs
	// between them, so at (t e, t e) it interpolated t * cap instead of the plane's 2 k e t: a slope ~29 % flatter.
	const float corner_pp = std::fmax(-cap, std::fmin(plane(e, e), cap));
	const float corner_mm = std::fmax(-cap, std::fmin(plane(-e, -e), cap));
	const float old_slope = (corner_pp - corner_mm) / (2.0f * e); // depth per unit of t * e along the diagonal
	const float exact_slope = 2.0f * k;
	CHECK(old_slope == doctest::Approx(exact_slope / std::sqrt(2.0f)).epsilon(1e-4));
	CHECK(old_slope < 0.75f * exact_slope);
	const float exact_half = caster_fragment_depth(0.0f, plane(0.5f * e, 0.5f * e), cap, false);
	CHECK(exact_half == doctest::Approx(exact_slope * 0.5f * e).epsilon(1e-4));
	// Beyond the cap (outside the ellipse) the depth is capped, never spiking.
	CHECK(caster_fragment_depth(center, plane(e, e), cap, false) == doctest::Approx(center + cap));
	CHECK(caster_fragment_depth(center, -10.0f, cap, false) == doctest::Approx(center - cap));
	// A flat quad (cap 0, e.g. pancaked) keeps the centre depth; a pancake pass never exceeds kPancakeDepth; the
	// written depth stays inside [0, 1].
	CHECK(caster_fragment_depth(0.3f, 0.2f, 0.0f, false) == 0.3f);
	CHECK(caster_fragment_depth(0.9998f, 0.01f, 0.01f, true) == kPancakeDepth);
	CHECK(caster_fragment_depth(0.9998f, 0.01f, 0.01f, false) == 1.0f);
	CHECK(caster_fragment_depth(0.001f, -0.01f, 0.01f, false) == 0.0f);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] a needle tilted across both axes keeps its depth slope in float32") {
	// The review case: scales (1, 1e-5, 1e-5), long axis v = (0.5, 0.5, sqrt(0.5)), 512 texels per unit, light depth
	// along z. Along the needle depth changes sqrt(0.5) per unit while the screen moves 512 * sqrt(0.5) texels, so the
	// slope along the projected axis (1, 1) / sqrt(2) is 1 / 512 (times s / (s + w) from the low-pass).
	const float texels = 512.0f;
	const float v[3] = { 0.5f, 0.5f, std::sqrt(0.5f) };
	const float minor2 = 1e-10f;
	float cov[3][3];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			cov[i][j] = (1.0f - minor2) * v[i] * v[j] + (i == j ? minor2 : 0.0f);
		}
	}
	const float a = texels * texels * cov[0][0];
	const float b = texels * texels * cov[0][1];
	const float c = texels * texels * cov[1][1];
	const float cx = texels * cov[0][2];
	const float cy = texels * cov[1][2];
	// The old adjugate / det form: det cancels to (near) zero, below its own relative threshold, and drops the plane.
	const float det = a * c - b * b;
	CHECK_FALSE(det > 1e-12f * a * c);
	const DepthGradient g = caster_depth_gradient(a, b, c, kLowPassTexels2, cx, cy);
	const float s = a + c; // the projected needle's variance along its axis
	const float expected = (1.0f / texels) * s / (s + kLowPassTexels2);
	const float along = (g.x + g.y) / std::sqrt(2.0f);
	const float across = (g.x - g.y) / std::sqrt(2.0f);
	CHECK(along == doctest::Approx(expected).epsilon(1e-3));
	// Across the needle the quad is only sqrt(minor) wide: the depth it spans there must be negligible next to the
	// depth span along the needle (rounding in the minor term is bounded by that width, then capped per fragment).
	const FootprintEigen e = footprint_eigenvalues(a, b, c, kLowPassTexels2);
	CHECK(std::fabs(across) * std::sqrt(e.minor) <= 1e-3f * along * std::sqrt(e.major));

	// A well-conditioned case equals S'^-1 c exactly (the textbook solve).
	const float wa = 5.0f, wb = 2.0f, wc = 3.0f, w = kLowPassTexels2, wx = 0.7f, wy = -0.4f;
	const DepthGradient h = caster_depth_gradient(wa, wb, wc, w, wx, wy);
	const float d = (wa + w) * (wc + w) - wb * wb;
	CHECK(h.x == doctest::Approx(((wc + w) * wx - wb * wy) / d).epsilon(1e-5));
	CHECK(h.y == doctest::Approx(((wa + w) * wy - wb * wx) / d).epsilon(1e-5));
	// Axis-aligned (b = 0), both orientations.
	const DepthGradient ax = caster_depth_gradient(9.0f, 0.0f, 1.0f, 0.0f, 3.0f, 2.0f);
	CHECK(ax.x == doctest::Approx(3.0f / 9.0f));
	CHECK(ax.y == doctest::Approx(2.0f / 1.0f));
	const DepthGradient ay = caster_depth_gradient(1.0f, 0.0f, 9.0f, 0.0f, 3.0f, 2.0f);
	CHECK(ay.x == doctest::Approx(3.0f / 1.0f));
	CHECK(ay.y == doctest::Approx(2.0f / 9.0f));
}

} // namespace TestHlodShadowCasters
