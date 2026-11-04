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
	build_row_casts(row_keys, caster_keys, row_casts);
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
	// A node with cast_shadow off (key 11, row 2) is drawn but casts nothing; the world row always casts.
	caster_keys.erase(11);
	build_row_casts(row_keys, caster_keys, row_casts);
	CHECK(row_casts[0] == 1u);
	CHECK(row_casts[2] == 0u);
	CHECK(admitted_instance_splats(region, demands, 4, admission, row_casts) == 1500);
	// No caster keys at all: only the world row casts.
	LocalVector<uint64_t> no_keys;
	build_row_casts(row_keys, no_keys, row_casts);
	CHECK(row_casts[0] == 1u);
	CHECK(admitted_instance_splats(region, demands, 4, admission, row_casts) == 0);
	// No objects admitted: no instance casters; an out-of-range demand index or row is ignored, never read.
	build_row_casts(row_keys, caster_keys, row_casts);
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

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] the camera-relative clip transform is Godot's directional shadow transform") {
	// A cascade like Godot builds it: orthographic, light looking down at a slant, origin near the camera.
	Projection ortho;
	ortho.set_orthogonal(-8.0f, 8.0f, -8.0f, 8.0f, 0.0f, 60.0f);
	Transform3D light;
	light.basis = Basis::from_euler(Vector3(Math::deg_to_rad(-60.0f), Math::deg_to_rad(20.0f), 0.0f));
	light.origin = Vector3(3.0f, 30.0f, 5.0f);
	const Projection world_clip = godot_directional_shadow_clip(ortho, light);
	// Reverse-Z with depth correction: the near plane maps to depth 1, the far plane to 0.
	const Vector3 forward = -light.basis.get_column(2);
	const Vector3 near_point = light.origin + forward * 0.0f;
	const Vector3 far_point = light.origin + forward * 60.0f;
	CHECK(world_clip.xform(near_point).z == doctest::Approx(1.0f).epsilon(1e-4));
	CHECK(world_clip.xform(far_point).z == doctest::Approx(0.0f).epsilon(1e-4));
	// Depth correction flips y (flip_y = true for Godot's directional shadow passes).
	Projection uncorrected = ortho * Projection(light.affine_inverse());
	const Vector3 up_point = light.origin + forward * 10.0f + light.basis.get_column(1) * 2.0f;
	CHECK(world_clip.xform(up_point).y == doctest::Approx(-uncorrected.xform(up_point).y).epsilon(1e-4));
	// A camera-relative position p (world = p + C) lands where the world position does, near and far from the origin.
	const Vector3 cameras[] = { Vector3(0, 0, 0), Vector3(1.5f, 2.0f, -3.0f), Vector3(1000.0f, 40.0f, -2000.0f) };
	for (const Vector3 &camera : cameras) {
		Transform3D moved = light;
		moved.origin += camera; // Cascades follow the camera.
		const Projection relative_clip = frame_relative_shadow_clip(ortho, moved, camera);
		const Projection moved_world_clip = godot_directional_shadow_clip(ortho, moved);
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

} // namespace TestHlodShadowCasters
