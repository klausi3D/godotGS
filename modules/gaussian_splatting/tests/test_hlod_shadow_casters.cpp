extern "C" int test_hlod_shadow_casters_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../renderer/hlod_shadow_caster_plan.h"
#include "../renderer/hlod_shadow_refresh.h"
#include "../renderer/gs_wind_bounds.h"
#include "../renderer/hlod_world_instances.h"
#include "servers/rendering_server.h"

#include <cmath>

// Splat shadow casters on the HLOD route (#1095, #1163; renderer/hlod_shadow_caster_plan.h). CPU only: the caster list a
// light pass draws (the last active main view's draw list), its refusals, the caster budget (exactly the view's admitted
// set), the per-frame accounting, and Godot's directional/spot/cube shadow conventions with camera-relative input.
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

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] current rows keep previous list identity and refuse count key or asset changes") {
	// P3: compile-error RED before current_rows_refusal/FrameRecord identity fields exist.
	gs_hlod_bundle::Rows rows;
	rows.instances.resize(3);
	rows.grading.resize(3);
	rows.asset_ids.push_back(0);
	rows.asset_ids.push_back(41);
	rows.asset_ids.push_back(42);
	LocalVector<uint64_t> keys;
	keys.push_back(0);
	keys.push_back(101);
	keys.push_back(102);
	FrameRecord record = active_record();
	record.row_keys = keys;
	record.row_asset_ids = rows.asset_ids;
	record.row_casts.resize(3);
	LightPassInfo light;
	light.kind = LightKind::DIRECTIONAL;
	light.orthographic = true;
	CHECK(current_rows_refusal(record, rows, keys) == Refusal::NONE);
	CHECK(plan_pass(record, light, true, current_rows_refusal(record, rows, keys)).draws());
	// A moving/graded/windy row retains its identity: current payload changes are the legal route.
	rows.instances[1].translation_scale[0] = 17.0f;
	rows.instances[1].rotation[3] = 1.0f;
	rows.instances[1].params[2] = 0.7f;
	CHECK(current_rows_refusal(record, rows, keys) == Refusal::NONE);
	for (uint32_t mutation = 0; mutation < 7; ++mutation) {
		gs_hlod_bundle::Rows changed = rows;
		LocalVector<uint64_t> changed_keys = keys;
		FrameRecord previous = record;
		switch (mutation) {
			case 0: changed.instances.resize(2); break;
			case 1: changed.grading.resize(2); break;
			case 2: changed.asset_ids.resize(2); break;
			case 3: changed_keys.resize(2); break;
			case 4: changed_keys[1] = 999; break; // Equal-size replacement must not alias the old list.
			case 5: changed.asset_ids[2] = 99; break;
			case 6: previous.row_casts.resize(2); break;
		}
		const Refusal refusal = current_rows_refusal(previous, changed, changed_keys);
		CHECK(refusal == Refusal::ROWS_MISMATCH);
		CHECK(String(refusal_reason(refusal)) == "shadow_caster_rows_mismatch");
		const PassPlan plan = plan_pass(previous, light, true, refusal);
		CHECK(plan.refusal == Refusal::ROWS_MISMATCH);
		CHECK(!plan.draws());
		CHECK(plan.casters == 0);
		CHECK(plan.instance_slots == 0);
	}
	CHECK(plan_pass(record, light, false, Refusal::NONE).refusal == Refusal::BUNDLE_CHANGED);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] row snapshot latches only for its exact server frame and bundle") {
	// Compile-error RED before the P3 once-per-RS-frame helper exists. A late director
	// edit keeps this frame's rows; the next frame or a replacement bundle must refresh.
	CHECK(rows_frame_latched(41, 7, 41, 7));
	CHECK_FALSE(rows_frame_latched(42, 7, 41, 7));
	CHECK_FALSE(rows_frame_latched(41, 8, 41, 7));
	CHECK_FALSE(rows_frame_latched(42, 8, 41, 7));
	CHECK_FALSE(rows_frame_latched(41, 7, 0, 0));
	CHECK(rows_frame_latched(42, 8, 42, 8));
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] releasing a latched or refused contract retains terminal refusal for that frame") {
	// Compile-error RED before retained_refusal_frame exists. Its booleans are the
	// current-frame predicates, so an unrelated release cannot revive an older refusal.
	CHECK(retained_refusal_frame(true, false, 41, UINT64_MAX) == 41u);
	CHECK(retained_refusal_frame(false, true, 41, 41) == 41u);
	CHECK(retained_refusal_frame(false, true, UINT64_MAX, 41) == 41u); // Repeated teardown after sync fields were reset.
	CHECK(retained_refusal_frame(false, false, UINT64_MAX, 41) == UINT64_MAX); // Next frame: neither predicate matches.
	CHECK(retained_refusal_frame(false, false, 40, 39) == UINT64_MAX); // Unrelated release clears stale history.
	CHECK(retained_refusal_frame(true, true, 42, 41) == 42u); // The newer successful latch owns the refusal frame.
	CHECK(retained_refusal_frame(true, false, 43, 41) == 43u);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] newly admitted rows are counted by stable key independently of world and record count") {
	// P3 compile-error RED before row_in_list/entered_rows exist. Rows are unique director keys;
	// many DrawnNodeGPU records for one object collapse to its single row_in_list bit.
	FrameRecord previous;
	previous.valid = true;
	previous.row_keys.push_back(0);
	previous.row_keys.push_back(101);
	previous.row_keys.push_back(102);
	previous.row_keys.push_back(103);
	previous.row_in_list.push_back(1);
	previous.row_in_list.push_back(1);
	previous.row_in_list.push_back(0);
	previous.row_in_list.push_back(0);
	FrameRecord current = previous;
	current.row_keys[1] = 102;
	current.row_keys[2] = 101; // Reordering an existing object does not make it newly entered.
	current.row_in_list[2] = 1;
	current.instance_records = 19; // Number of node records cannot multiply the per-object counter.
	CHECK(entered_rows(previous, current) == 1u);
	CHECK(entered_rows(current, current) == 0u);
	previous.row_in_list[0] = 0; // World entering the list is never counted as an entering object.
	CHECK(entered_rows(previous, current) == 1u);
	current.row_keys[3] = 999; // Visible but refused/unadmitted rows do not enter the previous-list caster set.
	CHECK(entered_rows(previous, current) == 1u);
	current.row_in_list[3] = 1;
	CHECK(entered_rows(previous, current) == 2u);
	const FrameRecord no_previous;
	CHECK(entered_rows(no_previous, current) == 3u);
	current.row_in_list[1] = current.row_in_list[2] = current.row_in_list[3] = 0;
	CHECK(entered_rows(previous, current) == 0u);
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

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] per-light totals distinguish cascades and refused lights and reset together") {
	FrameStats scalar;
	LightFrameStats per_light;
	const PassPlan cast = plan_pass(active_record(), true, true);
	const PassPlan refused = plan_pass(active_record(), false, true);
	// Use full-width RID IDs, including two whose low 32 bits match.
	const uint64_t a = (uint64_t(1) << 32) + 1;
	const uint64_t b = (uint64_t(2) << 32) + 1;
	const uint64_t c = 3;
	for (int pass = 0; pass < 4; ++pass) {
		scalar.record(41, cast);
		per_light.record(1041, 41, a, true, cast.casters);
	}
	scalar.record(41, cast);
	per_light.record(1041, 41, b, true, cast.casters);
	for (int pass = 0; pass < 6; ++pass) {
		scalar.record(41, refused);
		// Even a nonzero raster count must not count if the final atlas write failed.
		per_light.record(1041, 41, c, false, cast.casters);
	}
	scalar.record(41, refused);
	per_light.record(1041, 41, a, false, cast.casters);
	if (per_light.lights.size() != 3) {
		FAIL("expected three identified lights, got ", per_light.lights.size());
		return;
	}
	CHECK(per_light.renderer_frame == scalar.frame);
	CHECK(per_light.server_frame == 1041);
	CHECK(per_light.lights[0].id == a);
	CHECK(per_light.lights[0].casters == 4 * cast.casters);
	CHECK(per_light.lights[0].passes == 5);
	CHECK(per_light.lights[0].no_caster_passes == 1);
	CHECK(per_light.lights[1].id == b);
	CHECK(per_light.lights[1].casters == cast.casters);
	CHECK(per_light.lights[1].passes == 1);
	CHECK(per_light.lights[1].no_caster_passes == 0);
	CHECK(per_light.lights[0].casters != per_light.lights[1].casters);
	CHECK(per_light.lights[2].id == c);
	CHECK(per_light.lights[2].casters == 0);
	CHECK(per_light.lights[2].passes == 6);
	CHECK(per_light.lights[2].no_caster_passes == 6);
	CHECK(per_light.no_caster_lights() == 1); // Six refused C passes and one refused A pass, only one zero-total light.
	uint64_t sum = 0;
	for (const LightFrameStats::Light &light : per_light.lights) {
		sum += light.casters;
	}
	CHECK(sum == scalar.casters);
	CHECK(per_light.overflow_passes == 0);
	// C submits nothing next frame: it must disappear, not persist as a zero entry.
	scalar.record(42, cast);
	scalar.record(42, cast);
	per_light.record(1042, 42, a, true, cast.casters);
	per_light.record(1042, 42, b, true, cast.casters);
	if (per_light.lights.size() != 2) {
		FAIL("expected only A and B after frame change, got ", per_light.lights.size());
		return;
	}
	CHECK(per_light.renderer_frame == 42);
	CHECK(per_light.server_frame == 1042);
	CHECK(per_light.renderer_frame == scalar.frame);
	CHECK(per_light.lights[0].id == a);
	CHECK(per_light.lights[1].id == b);
	CHECK(per_light.lights[0].passes == 1);
	CHECK(per_light.lights[0].no_caster_passes == 0);
	CHECK(per_light.lights[0].casters + per_light.lights[1].casters == scalar.casters);
	CHECK(per_light.no_caster_lights() == 0);
	// A successful but empty pass is also a no-caster pass.
	per_light.record(1043, 43, b, true, 0);
	CHECK(per_light.no_caster_lights() == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] RS frame groups advancing canonical renderer counters and keeps the first stamp") {
	LightFrameStats stats;
	// Canonical raster advances the renderer counter between shadow passes. The RS key remains fixed.
	stats.record(1041, 80, 11, false, 123);
	stats.record(1041, 81, 22, false, 0);
	stats.record(1041, 82, 11, true, 7);
	if (stats.lights.size() != 2) {
		FAIL("renderer counter changes discarded a light within one RS frame: ", stats.lights.size());
		return;
	}
	CHECK(stats.server_frame == 1041);
	CHECK(stats.renderer_frame == 80);
	CHECK(stats.lights[0].id == 11);
	CHECK(stats.lights[0].casters == 7);
	CHECK(stats.lights[0].passes == 2);
	CHECK(stats.lights[0].no_caster_passes == 1);
	CHECK(stats.lights[1].id == 22);
	CHECK(stats.lights[1].casters == 0);
	CHECK(stats.no_caster_lights() == 1);
	// Even an unchanged renderer counter cannot retain entries across an RS frame transition.
	stats.record(1042, 82, 33, false, 0);
	if (stats.lights.size() != 1) {
		FAIL("new RS frame did not clear previous lights: ", stats.lights.size());
		return;
	}
	CHECK(stats.server_frame == 1042);
	CHECK(stats.renderer_frame == 82);
	CHECK(stats.lights[0].id == 33);
	CHECK(stats.lights[0].passes == 1);
	CHECK(stats.no_caster_lights() == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] per-light accounting ignores invalid IDs and retains 64-bit caster counts") {
	LightFrameStats per_light;
	CHECK(per_light.server_frame == UINT64_MAX);
	CHECK(per_light.renderer_frame == UINT64_MAX);
	CHECK(per_light.no_caster_lights() == 0);
	per_light.record(1040, 40, 0, false, 0);
	per_light.record(1040, 40, 0, true, 123);
	CHECK(per_light.server_frame == UINT64_MAX);
	CHECK(per_light.renderer_frame == UINT64_MAX);
	CHECK(per_light.lights.is_empty());
	CHECK(per_light.no_caster_lights() == 0);
	FrameStats scalar;
	PassPlan cast = plan_pass(active_record(), true, true);
	// Accounting accepts uint64_t counts independently of today's draw-slot limit.
	cast.casters = uint64_t(UINT32_MAX) + 17;
	scalar.record(41, cast);
	per_light.record(1041, 41, 7, true, cast.casters);
	per_light.record(1042, 42, 0, false, 0); // A legacy call cannot advance or erase identified-light evidence.
	if (per_light.lights.size() != 1) {
		FAIL("expected one identified light, got ", per_light.lights.size());
		return;
	}
	CHECK(per_light.renderer_frame == scalar.frame);
	CHECK(per_light.server_frame == 1041);
	CHECK(per_light.lights[0].id == 7);
	CHECK(per_light.lights[0].casters == cast.casters);
	CHECK(per_light.lights[0].casters == scalar.casters);
	CHECK(per_light.lights[0].passes == 1);
	CHECK(per_light.no_caster_lights() == 0);
	CHECK(per_light.overflow_passes == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] per-light cap counts omitted passes and resets without blocking known lights") {
	LightFrameStats stats;
	for (uint32_t id = 1; id <= LightFrameStats::kMaxLights; ++id) {
		stats.record(1041, 41, id, true, id);
	}
	const uint64_t extra_id = LightFrameStats::kMaxLights + 1;
	stats.record(1041, 41, extra_id, false, 0);
	stats.record(1041, 41, extra_id, true, 100);
	stats.record(1041, 41, 0, false, 0); // Invalid IDs do not consume capacity or increment overflow.
	stats.record(1041, 41, 1, true, 10);
	if (stats.lights.size() != LightFrameStats::kMaxLights) {
		FAIL("light storage exceeded or failed to reach its cap: ", stats.lights.size());
		return;
	}
	CHECK(stats.overflow_passes == 2);
	CHECK(stats.lights[0].id == 1);
	CHECK(stats.lights[0].casters == 11);
	CHECK(stats.lights[0].passes == 2);
	CHECK(stats.no_caster_lights() == 0); // An omitted light is not an observed zero.
	for (const LightFrameStats::Light &light : stats.lights) {
		CHECK(light.id != extra_id);
	}
	stats.record(1042, 42, extra_id, false, 100);
	if (stats.lights.size() != 1) {
		FAIL("expected one light after resetting a full frame, got ", stats.lights.size());
		return;
	}
	CHECK(stats.renderer_frame == 42);
	CHECK(stats.server_frame == 1042);
	CHECK(stats.overflow_passes == 0);
	CHECK(stats.lights[0].id == extra_id);
	CHECK(stats.lights[0].casters == 0);
	CHECK(stats.lights[0].passes == 1);
	CHECK(stats.lights[0].no_caster_passes == 1);
	CHECK(stats.no_caster_lights() == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] explicit positional types refuse even an identity projection") {
	LightPassInfo pass;
	pass.orthographic = Projection().is_orthogonal(); // Cube face 0 after mesh finalization, also dual paraboloids.
	const FrameRecord record = active_record();
	CHECK(plan_pass(record, pass.supports_hlod_casters(), true).draws()); // Legacy unknown caller compatibility.
	pass.light_type = RS::LIGHT_DIRECTIONAL;
	pass.kind = LightKind::DIRECTIONAL;
	CHECK(plan_pass(record, pass.supports_hlod_casters(), true).draws());
	for (LightKind kind : { LightKind::OMNI_CUBE, LightKind::OMNI_PARABOLOID, LightKind::SPOT, LightKind::UNKNOWN }) {
		pass.kind = kind;
		pass.light_type = kind == LightKind::SPOT ? RS::LIGHT_SPOT : RS::LIGHT_OMNI;
		const PassPlan plan = plan_pass(record, pass.supports_hlod_casters(), true);
		CHECK_FALSE(plan.draws());
		CHECK(plan.refusal == Refusal::NOT_DIRECTIONAL);
	}
	pass.light_type = RS::LIGHT_DIRECTIONAL;
	pass.kind = LightKind::DIRECTIONAL;
	pass.orthographic = false;
	CHECK(plan_pass(record, pass.supports_hlod_casters(), true).refusal == Refusal::NOT_DIRECTIONAL);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] perspective spot and all six omni cube passes cast the admitted list") {
	const FrameRecord record = active_record();
	for (LightKind kind : { LightKind::SPOT, LightKind::OMNI_CUBE }) {
		LightPassInfo pass;
		pass.kind = kind;
		pass.light_type = kind == LightKind::SPOT ? RS::LIGHT_SPOT : RS::LIGHT_OMNI;
		pass.omni_shadow_mode = kind == LightKind::OMNI_CUBE ? RS::LIGHT_OMNI_SHADOW_CUBE : -1;
		pass.orthographic = false;
		const int pass_count = kind == LightKind::OMNI_CUBE ? 6 : 1;
		for (int index = 0; index < pass_count; ++index) {
			pass.pass_index = index;
			CHECK_MESSAGE(pass.supports_hlod_casters(), light_kind_name(kind), " pass ", index);
			// Deliberately use the existing bool API: before the fix this compiles and fails with NOT_DIRECTIONAL.
			const PassPlan plan = plan_pass(record, pass.supports_hlod_casters(), true);
			CHECK(plan.draws());
			CHECK(plan.refusal == Refusal::NONE);
			CHECK(plan.casters == record.casters());
			CHECK(plan.instance_slots == record.draw_count * record.max_chunk_splats);
		}
		FrameRecord quantized = record;
		quantized.quantized = true;
		CHECK(plan_pass(quantized, pass.supports_hlod_casters(), true).refusal == Refusal::QUANTIZED_PAYLOAD);
		FrameRecord empty = record;
		empty.draw_count = 0;
		CHECK(plan_pass(empty, pass.supports_hlod_casters(), true).refusal == Refusal::EMPTY_DRAW_LIST);
		CHECK(plan_pass(FrameRecord(), pass.supports_hlod_casters(), true).refusal == Refusal::NO_ACTIVE_HLOD_VIEW);
		CHECK(plan_pass(record, pass.supports_hlod_casters(), false).refusal == Refusal::BUNDLE_CHANGED);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] omni paraboloid passes name their unsupported projection") {
	LightPassInfo pass;
	pass.kind = LightKind::OMNI_PARABOLOID;
	pass.light_type = RS::LIGHT_OMNI;
	pass.omni_shadow_mode = RS::LIGHT_OMNI_SHADOW_DUAL_PARABOLOID;
	pass.orthographic = true; // Godot supplies an identity projection for both hemispheres.
	for (int hemisphere = 0; hemisphere < 2; ++hemisphere) {
		pass.pass_index = hemisphere;
		const PassPlan plan = plan_pass(active_record(), pass, true);
		CHECK_FALSE(plan.draws());
		CHECK(plan.refusal == Refusal::OMNI_PARABOLOID_UNSUPPORTED);
		CHECK(String(refusal_reason(plan.refusal)) == "shadow_caster_omni_paraboloid_unsupported");
		CHECK(plan.casters == 0);
		CHECK(plan.instance_slots == 0);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] per-light named refusal survives other lights and snapshot but resets next frame") {
	LightFrameStats stats;
	LightPassInfo paraboloid;
	paraboloid.kind = LightKind::OMNI_PARABOLOID;
	paraboloid.light_type = RS::LIGHT_OMNI;
	paraboloid.omni_shadow_mode = RS::LIGHT_OMNI_SHADOW_DUAL_PARABOLOID;
	paraboloid.orthographic = true;
	paraboloid.pass_index = 0;
	stats.record(1041, 41, 11, false, 0, paraboloid, Refusal::OMNI_PARABOLOID_UNSUPPORTED);
	LightPassInfo spot;
	spot.kind = LightKind::SPOT;
	spot.light_type = RS::LIGHT_SPOT;
	spot.pass_index = 0;
	stats.record(1041, 41, 22, true, 123, spot);
	paraboloid.pass_index = 1;
	stats.record(1041, 41, 11, false, 0, paraboloid, Refusal::OMNI_PARABOLOID_UNSUPPORTED);
	const LightFrameStats::Snapshot refused = stats.snapshot();
	if (refused.lights.size() != 2) {
		FAIL("expected the refused omni and casting spot in the snapshot, got ", refused.lights.size());
		return;
	}
	CHECK(refused.lights[0].id == 11);
	CHECK(refused.lights[0].last_refusal == Refusal::OMNI_PARABOLOID_UNSUPPORTED);
	CHECK(String(refusal_reason(refused.lights[0].last_refusal)) == "shadow_caster_omni_paraboloid_unsupported");
	CHECK(refused.lights[0].passes == 2);
	CHECK(refused.lights[0].no_caster_passes == 2);
	CHECK(refused.lights[0].casters == 0);
	CHECK(refused.no_caster_count == 1); // Naming the refusal does not turn a no-caster observation into a cast.
	CHECK(refused.lights[1].casters == 123);
	CHECK(refused.lights[1].last_refusal == Refusal::NONE);
	// No new refusal on an unrelated light can clear the omni's named diagnostic in this RS frame.
	stats.record(1041, 42, 22, true, 456, spot);
	CHECK(stats.lights[0].last_refusal == Refusal::OMNI_PARABOLOID_UNSUPPORTED);
	CHECK(stats.no_caster_lights() == 1);
	// The same RID with a supported projection next frame must not retain the old mode's refusal.
	LightPassInfo cube = paraboloid;
	cube.kind = LightKind::OMNI_CUBE;
	cube.omni_shadow_mode = RS::LIGHT_OMNI_SHADOW_CUBE;
	cube.orthographic = false;
	stats.record(1042, 42, 11, true, 789, cube);
	if (stats.lights.size() != 1) {
		FAIL("new frame must contain only the current light, got ", stats.lights.size());
		return;
	}
	CHECK(stats.lights[0].last_refusal == Refusal::NONE);
	CHECK(stats.lights[0].casters == 789);
	CHECK(stats.no_caster_lights() == 0);
	CHECK(refused.lights[0].last_refusal == Refusal::OMNI_PARABOLOID_UNSUPPORTED); // Snapshot owns the diagnostic too.
	CHECK(refused.no_caster_count == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] snapshot owns its values across later records and frame resets") {
	LightFrameStats stats;
	LightPassInfo pass;
	pass.kind = LightKind::DIRECTIONAL;
	pass.light_type = RS::LIGHT_DIRECTIONAL;
	pass.orthographic = true;
	pass.pass_index = 0;
	stats.record(1041, 41, 7, true, 100, pass);
	pass.pass_index = 1;
	stats.record(1041, 41, 7, false, 100, pass);
	pass.kind = LightKind::OMNI_CUBE;
	pass.light_type = RS::LIGHT_OMNI;
	pass.omni_shadow_mode = RS::LIGHT_OMNI_SHADOW_CUBE;
	for (int face = 0; face < 6; ++face) {
		pass.pass_index = face;
		pass.orthographic = face == 0;
		stats.record(1041, 41, 8, false, 100, pass);
	}
	const LightFrameStats::Snapshot published = stats.snapshot();
	LightFrameStats::Snapshot reader;
	reader = published; // Same assignment used under the publication mutex by build_render_stats().
	stats.record(1041, 41, 7, true, 900, pass); // Mutate an existing entry and its diagnostic kind.
	for (uint32_t id = 9; id < 9 + LightFrameStats::kMaxLights; ++id) {
		stats.record(1041, 41, id, true, 1, pass); // Grow/reallocate storage and overflow.
	}
	const LightFrameStats::Snapshot full = stats.snapshot();
	stats.record(1042, 42, 99, true, 77, pass); // Clear and reuse recorder storage.
	if (published.lights.size() != 2 || reader.lights.size() != 2 || stats.lights.size() != 1) {
		FAIL("snapshot or recorder cardinality changed unexpectedly");
		return;
	}
	CHECK(reader.renderer_frame == 41);
	CHECK(reader.overflow_passes == 0);
	CHECK(reader.no_caster_count == 1);
	CHECK(reader.lights[0].id == 7);
	CHECK(reader.lights[0].casters == 100);
	CHECK(reader.lights[0].passes == 2);
	CHECK(reader.lights[0].cast_passes == 1);
	CHECK(reader.lights[0].ortho_passes == 2);
	CHECK(reader.lights[0].kind == LightKind::DIRECTIONAL);
	CHECK(reader.lights[0].last_pass.pass_index == 1);
	CHECK(reader.lights[1].id == 8);
	CHECK(reader.lights[1].casters == 0);
	CHECK(reader.lights[1].passes == 6);
	CHECK(reader.lights[1].cast_passes == 0);
	CHECK(reader.lights[1].ortho_passes == 1);
	CHECK(reader.lights[1].kind == LightKind::OMNI_CUBE);
	CHECK(reader.lights[1].last_pass.pass_index == 5);
	CHECK(reader.lights[1].last_pass.light_type == RS::LIGHT_OMNI);
	CHECK(reader.lights[1].last_pass.omni_shadow_mode == RS::LIGHT_OMNI_SHADOW_CUBE);
	CHECK(full.overflow_passes == 2);
	CHECK(stats.renderer_frame == 42);
	CHECK(stats.lights[0].casters == 77);
	reader.lights[0].casters = 999;
	CHECK(published.lights[0].casters == 100); // Reader also owns storage independently of the published value.
	CHECK(String(light_kind_name(LightKind::DIRECTIONAL)) == "directional");
	CHECK(String(light_kind_name(LightKind::OMNI_CUBE)) == "omni_cube");
	CHECK(String(light_kind_name(LightKind::OMNI_PARABOLOID)) == "omni_paraboloid");
	CHECK(String(light_kind_name(LightKind::SPOT)) == "spot");
	CHECK(String(light_kind_name(LightKind::UNKNOWN)) == "unknown");
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

// Receiver references stay in this test file and do not call the caster's transform helpers. Spot receivers use
// light_storage.cpp's bias * correction(false, true, false) * projection * inverse-light, with the atlas rect
// applied after perspective division by scene_forward_lights_inc.glsl. Projection::xform(Vector3) divides by w.
static Vector3 spot_receiver_sample(const Projection &p_projection, const Transform3D &p_light,
		const Rect2 &p_rect, const Vector3 &p_world) {
	Projection receiver_correction;
	receiver_correction.set_depth_correction(false, true, false);
	Projection receiver_bias;
	receiver_bias.set_light_bias();
	const Projection receiver_matrix = receiver_bias * receiver_correction * p_projection * Projection(p_light.affine_inverse());
	const Vector3 sample = receiver_matrix.xform(p_world);
	return Vector3(p_rect.position.x + sample.x * p_rect.size.x,
			p_rect.position.y + sample.y * p_rect.size.y, sample.z);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] spot perspective casters match Godot receiver uv and reverse depth") {
	const float near_plane = 0.025f;
	const float range = 32.0f;
	Projection perspective;
	perspective.set_perspective(80.0f, 1.0f, near_plane, range); // Godot's twice-spot-angle projection.
	Transform3D light;
	light.basis = Basis::from_euler(Vector3(-0.55f, 0.32f, 0.11f));
	light.origin = Vector3(3.0f, 12.0f, -4.0f);
	const Rect2 rect(0.5f, 0.25f, 0.25f, 0.25f);
	const Vector3 local_points[] = { Vector3(1.5f, 2.0f, -6.0f), Vector3(-3.0f, -1.5f, -12.0f), Vector3(2.0f, -4.0f, -20.0f) };
	const Vector3 cameras[] = { Vector3(), Vector3(128.0f, -32.0f, 80.0f) };
	const float uv_tolerance = 0.5f / 4096.0f; // Half an atlas texel, not an arbitrary world-space epsilon.
	const float depth_tolerance = 1.0f / 65536.0f;
	Projection orthographic;
	orthographic.set_orthogonal(-8.0f, 8.0f, -8.0f, 8.0f, near_plane, range);
	const Projection wrong_ortho = godot_shadow_clip(orthographic, light, true);
	const Projection wrong_flip = godot_shadow_clip(perspective, light, false);
	const Projection wrong_depth = perspective * Projection(light.affine_inverse()); // Raw OpenGL forward [-1,1] depth.
	for (const Vector3 &local : local_points) {
		const Vector3 world = light.xform(local);
		const Vector3 receiver = spot_receiver_sample(perspective, light, rect, world);
		for (const Vector3 &camera : cameras) {
			const Projection clip = frame_relative_shadow_clip(perspective, light, true, camera);
			const Vector3 caster = caster_atlas_sample(clip, rect, world - camera);
			CHECK(std::fabs(caster.x - receiver.x) <= uv_tolerance);
			CHECK(std::fabs(caster.y - receiver.y) <= uv_tolerance);
			CHECK(std::fabs(caster.z - receiver.z) <= depth_tolerance);
		}
		// Each forcing case must miss this independent receiver reference for every off-axis point.
		const Vector3 ortho_sample = caster_atlas_sample(wrong_ortho, rect, world);
		const bool ortho_misses_uv = std::fabs(ortho_sample.x - receiver.x) > uv_tolerance || std::fabs(ortho_sample.y - receiver.y) > uv_tolerance;
		CHECK(ortho_misses_uv);
		CHECK(std::fabs(ortho_sample.z - receiver.z) > depth_tolerance);
		CHECK(std::fabs(caster_atlas_sample(wrong_flip, rect, world).y - receiver.y) > uv_tolerance);
		CHECK(std::fabs(caster_atlas_sample(wrong_depth, rect, world).z - receiver.z) > depth_tolerance);
	}
	const Projection clip = godot_shadow_clip(perspective, light, true);
	CHECK(clip.xform(light.xform(Vector3(0, 0, -near_plane))).z == doctest::Approx(1.0f).epsilon(1e-3));
	CHECK(std::fabs(clip.xform(light.xform(Vector3(0, 0, -range))).z) <= depth_tolerance);
}

// The unchanged omni receiver first transforms world to BASE light space, then uses radial reverse depth and
// stereographic xy/(1+abs(z)), with one atlas texel inset (scene_forward_lights_inc.glsl). Positive Z selects the
// second atlas cell; neither hemisphere flips X. Bias and PCF are zero here so the sampled point is unambiguous.
static Vector3 omni_receiver_sample(const Transform3D &p_base_light, const Rect2 &p_first_cell,
		const Vector2 &p_second_cell_offset, float p_atlas_size, float p_range, const Vector3 &p_world) {
	const Vector3 q = p_base_light.affine_inverse().xform(p_world);
	const float distance = q.length();
	const Vector3 direction = q / distance;
	const Vector2 chart(direction.x / (1.0f + std::fabs(direction.z)), direction.y / (1.0f + std::fabs(direction.z)));
	Rect2 cell = p_first_cell;
	if (direction.z >= 0.0f) {
		cell.position += p_second_cell_offset;
	}
	const float texel = 1.0f / p_atlas_size;
	return Vector3(cell.position.x + texel + (chart.x * 0.5f + 0.5f) * (cell.size.x - 2.0f * texel),
			cell.position.y + texel + (chart.y * 0.5f + 0.5f) * (cell.size.y - 2.0f * texel), 1.0f - distance / p_range);
}

// Follow a splat's cube-face write through Godot's existing cube_to_dp.glsl copy. Recover the face ray from its
// written UV (undo correction(true)'s Y flip), rotate to base-light space, reconstruct perspective axis distance,
// then apply the copy shader's 1/max(abs(direction)) radial-depth correction and its inverse one-texel UV expansion.
// This consumes the caster's output, never the receiver's world point or receiver shadow matrix.
static Vector3 cube_caster_sample_after_copy(const Projection &p_clip, const Vector3 &p_relative_point,
		const Transform3D &p_base_light, const Transform3D &p_face_light, const Rect2 &p_first_cell,
		const Vector2 &p_second_cell_offset, float p_atlas_size, float p_near, float p_range) {
	const Vector3 face_sample = caster_atlas_sample(p_clip, Rect2(0, 0, 1, 1), p_relative_point);
	const Vector3 face_ray(2.0f * face_sample.x - 1.0f, 1.0f - 2.0f * face_sample.y, -1.0f);
	const Vector3 direction = p_base_light.basis.inverse().xform(p_face_light.basis.xform(face_ray)).normalized();
	const float major = std::fmax(std::fabs(direction.x), std::fmax(std::fabs(direction.y), std::fabs(direction.z)));
	const double forward_depth = 2.0 * face_sample.z - 1.0;
	const double near_plane = p_near;
	const double far_plane = p_range;
	const double axis_distance = 2.0 * near_plane * far_plane / (far_plane + near_plane + forward_depth * (far_plane - near_plane));
	const float radial_depth = 1.0f - float(axis_distance / major) / p_range;
	const Vector2 chart(direction.x / (1.0f + std::fabs(direction.z)), direction.y / (1.0f + std::fabs(direction.z)));
	Rect2 cell = p_first_cell;
	if (direction.z >= 0.0f) {
		cell.position += p_second_cell_offset;
	}
	const float texel_x = 1.0f / (cell.size.x * p_atlas_size);
	const float texel_y = 1.0f / (cell.size.y * p_atlas_size);
	const Vector2 copy_uv(((chart.x * 0.5f + 0.5f) + texel_x) / (1.0f + 2.0f * texel_x),
			((chart.y * 0.5f + 0.5f) + texel_y) / (1.0f + 2.0f * texel_y));
	return Vector3(cell.position.x + copy_uv.x * cell.size.x, cell.position.y + copy_uv.y * cell.size.y, radial_depth);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] every omni cube face reaches Godot receiver atlas uv and radial depth") {
	const float near_plane = 0.025f;
	const float range = 32.0f;
	const float atlas_size = 4096.0f;
	const float uv_tolerance = 0.5f / atlas_size;
	// Decoding the float32 perspective depth magnifies its rounding near the far plane; four 16-bit depth units
	// still distinguish the wrong orthographic/forward-depth conventions by several orders of magnitude.
	const float depth_tolerance = 4.0f / 65536.0f;
	Projection perspective;
	perspective.set_perspective(90.0f, 1.0f, near_plane, range);
	Projection orthographic;
	orthographic.set_orthogonal(-8.0f, 8.0f, -8.0f, 8.0f, near_plane, range);
	Transform3D base_light;
	base_light.basis = Basis::from_euler(Vector3(-0.45f, 0.23f, 0.17f));
	base_light.origin = Vector3(3.0f, 12.0f, -4.0f);
	// Exactly renderer_scene_cull.cpp's face order and bases, independently of the caster implementation.
	const Vector3 normals[] = { Vector3(+1, 0, 0), Vector3(-1, 0, 0), Vector3(0, -1, 0), Vector3(0, +1, 0), Vector3(0, 0, +1), Vector3(0, 0, -1) };
	const Vector3 ups[] = { Vector3(0, -1, 0), Vector3(0, -1, 0), Vector3(0, 0, -1), Vector3(0, 0, +1), Vector3(0, -1, 0), Vector3(0, -1, 0) };
	// Horizontal and vertical atlas allocations both occur; neither may assume a hard-coded second-cell axis.
	const Rect2 first_cell(0.25f, 0.25f, 0.25f, 0.25f);
	const Vector2 second_offsets[] = { Vector2(0.25f, 0.0f), Vector2(0.0f, 0.25f) };
	const Vector3 cameras[] = { Vector3(), Vector3(128.0f, -32.0f, 80.0f) };
	for (int face = 0; face < 6; ++face) {
		const Transform3D face_light = base_light * Transform3D().looking_at(normals[face], ups[face]);
		const Transform3D wrong_face = base_light * Transform3D().looking_at(normals[(face + 1) % 6], ups[(face + 1) % 6]);
		// Nonaxial rays exercise the perspective divide, both UV axes, and the cube-to-radial depth correction.
		for (float distance : { 6.0f, 12.0f, 20.0f }) {
			const Vector3 world = face_light.xform(Vector3(0.25f * distance, -0.35f * distance, -distance));
			for (const Vector2 &offset : second_offsets) {
				const Vector3 receiver = omni_receiver_sample(base_light, first_cell, offset, atlas_size, range, world);
				for (const Vector3 &camera : cameras) {
					const Projection clip = frame_relative_shadow_clip(perspective, face_light, false, camera);
					const Vector3 caster = cube_caster_sample_after_copy(clip, world - camera, base_light, face_light, first_cell, offset, atlas_size, near_plane, range);
					CHECK_MESSAGE(std::fabs(caster.x - receiver.x) <= uv_tolerance, "face ", face);
					CHECK_MESSAGE(std::fabs(caster.y - receiver.y) <= uv_tolerance, "face ", face);
					CHECK_MESSAGE(std::fabs(caster.z - receiver.z) <= depth_tolerance, "face ", face);
				}
				const Vector3 wrong_ortho = cube_caster_sample_after_copy(godot_shadow_clip(orthographic, face_light, false), world,
						base_light, face_light, first_cell, offset, atlas_size, near_plane, range);
				CHECK((std::fabs(wrong_ortho.x - receiver.x) > uv_tolerance || std::fabs(wrong_ortho.y - receiver.y) > uv_tolerance));
				CHECK(std::fabs(wrong_ortho.z - receiver.z) > depth_tolerance);
				const Vector3 wrong_flip = cube_caster_sample_after_copy(godot_shadow_clip(perspective, face_light, true), world,
						base_light, face_light, first_cell, offset, atlas_size, near_plane, range);
				CHECK((std::fabs(wrong_flip.x - receiver.x) > uv_tolerance || std::fabs(wrong_flip.y - receiver.y) > uv_tolerance));
				const Vector3 wrong_face_sample = cube_caster_sample_after_copy(godot_shadow_clip(perspective, face_light, false), world,
						base_light, wrong_face, first_cell, offset, atlas_size, near_plane, range);
				CHECK((std::fabs(wrong_face_sample.x - receiver.x) > uv_tolerance || std::fabs(wrong_face_sample.y - receiver.y) > uv_tolerance));
				const Projection raw_depth = perspective * Projection(face_light.affine_inverse());
				const Vector3 wrong_depth = cube_caster_sample_after_copy(raw_depth, world, base_light, face_light, first_cell, offset, atlas_size, near_plane, range);
				CHECK(std::fabs(wrong_depth.z - receiver.z) > depth_tolerance);
			}
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

	// Nearly axis-aligned needle (review case): a = 2621440000, b = 512000, c = 100. lambda0 - a rounds to 0, so the
	// (b, lambda0 - a) eigenvector lost the tilt (~1.95e-4). With c parallel to the true major axis, g must be too.
	{
		const float na = 2621440000.0f, nb = 512000.0f, nc = 100.0f;
		const float tilt = nb / (na - nc);
		const float nx = 1000.0f, ny = 1000.0f * tilt;
		const DepthGradient n = caster_depth_gradient(na, nb, nc, kLowPassTexels2, nx, ny);
		CHECK(n.y / n.x == doctest::Approx(tilt).epsilon(1e-2));
	}

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

// Existing [HLOD] is a strict host lane. These cases exercise production helpers without a device or scene tree.
// Synthetic world with world-sized owner bounds and two independently casting rows/records.
struct RefreshFixture {
	gs_shadow_refresh::PublishedContent content;
	gs_shadow_refresh::ContentIndex index;
	FrameRecord record;
	DrawnNodeGPU draws[2]{};
	LocalVector<gs_shadow_refresh::Geometry> geometry;
	LocalVector<InstanceDataGPU> rows;
	LocalVector<real_t> margins;
	LocalVector<uint32_t> animated;
	LocalVector<gs_shadow_refresh::Owner> owners;
	RefreshFixture() {
		record.valid = true;
		record.draw_count = 2;
		for (uint32_t i = 0; i < 2; ++i) {
			record.row_casts.push_back(1);
			record.row_keys.push_back(42 + i);
			record.row_asset_ids.push_back(i);
			InstanceDataGPU row{};
			row.rotation[3] = row.inv_rotation[3] = row.translation_scale[3] = 1;
			rows.push_back(row);
			margins.push_back(0);
			animated.push_back(0);
			gs_shadow_refresh::Geometry g;
			g.bounds = AABB(Vector3(i == 0 ? -1 : 99, -1, -1), Vector3(2, 2, 2));
			g.payload = 1;
			g.chunk_key = i;
			geometry.push_back(g);
			draws[i].instance_id = i;
			draws[i].chunk_id = i;
			draws[i].fade = 1;
		}
		gs_shadow_refresh::Owner owner;
		owner.owner = 42;
		owner.bounds = AABB(Vector3(-1000, -1000, -1000), Vector3(2000, 2000, 2000));
		owner.content = &index;
		owners.push_back(owner);
		publish();
	}
	void publish() { content.publish(record, draws, geometry); rebuild(); }
	void rebuild() { index.rebuild(content, record, rows, margins, animated); }
	gs_shadow_refresh::LightUpdate query(const gs_shadow_refresh::LightState &p_light) const {
		return gs_shadow_refresh::prepare_light(owners, Vector3(), 2, p_light);
	}
};

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] static omni ignores moving anchor and far cut but refreshes near cut") {
	RefreshFixture f;
	f.draws[1].instance_id = 0; // Far and near cuts belong to the SAME world row and world-sized owner.
	f.publish();
	gs_shadow_refresh::LightState light, never_observed;
	const auto first = f.query(light);
	CHECK(first.refresh);
	light.commit(first);
	CHECK_FALSE(f.query(light).refresh);
	CHECK(f.query(never_observed).refresh);
	CHECK(f.query(never_observed).refresh); // No scheduled pass: no observation consumed.
	++f.record.draw_list_upload;
	++f.record.renderer_frame;
	f.publish();
	CHECK_FALSE(f.query(light).refresh);
	f.record.camera = Vector3(11, -7, 23);
	for (auto &draw : f.draws) { draw.camera_offset[0] = -11; draw.camera_offset[1] = 7; draw.camera_offset[2] = -23; }
	f.publish();
	CHECK_FALSE(f.query(light).refresh); // Same world geometry at another anchor.
	++f.draws[1].chunk_id; // Far cut changes while camera moves again.
	++f.geometry[1].chunk_key;
	f.record.camera.x += 3;
	f.draws[0].camera_offset[0] -= 3;
	f.draws[1].camera_offset[0] -= 3;
	f.publish();
	CHECK_FALSE(f.query(light).refresh);
	++f.draws[0].chunk_id;
	++f.geometry[0].chunk_key;
	f.publish();
	CHECK(f.query(light).refresh);
	light.commit(f.query(light));
	CHECK_FALSE(f.query(light).refresh);
	++f.geometry[1].payload; // Unrelated leaf edit, not a global source revision.
	f.publish();
	CHECK_FALSE(f.query(light).refresh);
	++f.geometry[0].payload;
	f.publish();
	CHECK(f.query(light).refresh);
	light.commit(f.query(light));
	f.rows[1].translation_scale[0] = 1; // Current far row changes during idle restore.
	f.rebuild();
	CHECK_FALSE(f.query(light).refresh);
	f.rows[0].params[0] = 0.75f;
	f.rebuild();
	CHECK(f.query(light).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] dense chunk row and legacy camera LOD remaps preserve light content") {
	RefreshFixture f;
	f.draws[0].instance_id = 1; // The near record is an object; its stable row/asset keys stay fixed.
	f.draws[1].instance_id = 0;
	f.publish();
	gs_shadow_refresh::LightState light;
	light.commit(f.query(light));
	f.draws[0].chunk_id += 123; // A far lower-ID asset grew the preceding chunk table.
	f.rows[1].ids[0] += 10;
	f.rows[1].lod[0] = 3;
	f.rows[1].params[1] = 2;
	f.publish();
	CHECK_FALSE(f.query(light).refresh);
	f.rows.push_back(f.rows[1]); // The same object moves to another dense row.
	f.record.row_keys.push_back(f.record.row_keys[1]);
	f.record.row_asset_ids.push_back(f.record.row_asset_ids[1]);
	f.record.row_casts.push_back(1);
	f.record.row_casts[1] = 0;
	f.margins.push_back(0);
	f.animated.push_back(0);
	f.draws[0].instance_id = 2;
	f.publish();
	CHECK_FALSE(f.query(light).refresh);
	++f.geometry[0].chunk_key; // A different chunk of this asset is a real content change.
	f.publish();
	CHECK(f.query(light).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] large-coordinate record and aggregate bounds retain their positive edge") {
	const AABB world = gs_shadow_refresh::enclosing_bounds(Vector3(-200000000, -1, -1), Vector3(1, 1, 1));
	CHECK(world.position.x + world.size.x >= real_t(1));
	gs_shadow_refresh::ContentIndex index;
	gs_shadow_refresh::SpatialRecord far, near;
	far.bounds = AABB(Vector3(-200000000, 0, 0), Vector3());
	far.digest = 10;
	near.bounds = AABB(Vector3(1, 0, 0), Vector3());
	near.digest = 20;
	index.records.push_back(far);
	index.records.push_back(near);
	index.build();
	if (index.cells.is_empty()) { FAIL("index must contain both records"); return; }
	CHECK(index.cells[0].bounds.position.x + index.cells[0].bounds.size.x >= real_t(1));
	uint64_t digest = 0;
	uint32_t count = 0;
	bool animated = false;
	index.query(0, Vector3(1, 0, 0), 0, digest, count, animated);
	CHECK(count == 1);
	CHECK(digest == 20);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] noncasting records cannot animate or dirty per-light content") {
	RefreshFixture f;
	f.record.row_casts[1] = 0;
	f.publish();
	gs_shadow_refresh::LightState light;
	light.commit(f.query(light));
	f.geometry[1].bounds = f.geometry[0].bounds;
	f.animated[1] = 1;
	f.rows[1].translation_scale[0] = 1;
	++f.draws[1].chunk_id;
	f.publish();
	CHECK_FALSE(f.query(light).refresh);
	gs::settings::GSWindSettings wind;
	wind.enabled = true;
	wind.strength = 1;
	f.rows[1].params[2] = 1;
	CHECK_FALSE(gs_wind::wind_active_on_casting_row(wind, f.rows[1], false));
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] swapping cast flags between byte-identical rows still refreshes positional shadows") {
	// WP6 (P3) case, rewritten for the per-light key of P1c: the same row payload and bounds, only the cast flags swap.
	RefreshFixture f;
	f.rows[1] = f.rows[0];
	f.geometry[1].bounds = f.geometry[0].bounds;
	f.record.row_casts[0] = 1;
	f.record.row_casts[1] = 0;
	f.publish();
	gs_shadow_refresh::LightState light;
	light.commit(f.query(light));
	CHECK_FALSE(f.query(light).refresh);
	f.record.row_casts[0] = 0;
	f.record.row_casts[1] = 1;
	f.publish();
	const auto swapped = f.query(light);
	CHECK(swapped.refresh);
	light.commit(swapped);
	CHECK_FALSE(f.query(light).refresh);
	// Returning to the original flags is another change even though the row payload remains identical.
	f.record.row_casts[0] = 1;
	f.record.row_casts[1] = 0;
	f.publish();
	CHECK(f.query(light).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] wind and effectors refresh only casting records near the light") {
	RefreshFixture f;
	gs_shadow_refresh::LightState light;
	light.commit(f.query(light));
	gs::settings::GSWindSettings wind;
	wind.enabled = true;
	wind.strength = 1;
	wind.direction = Vector3(1, 0, 0);
	f.rows[1].params[2] = 1;
	f.animated[1] = gs_wind::wind_active_on_casting_row(wind, f.rows[1], true);
	CHECK(f.animated[1] == 1);
	f.rebuild();
	CHECK_FALSE(f.query(light).refresh);
	f.rows[0].params[2] = 1;
	f.animated[0] = gs_wind::wind_active_on_casting_row(wind, f.rows[0], true);
	f.rebuild();
	CHECK(f.query(light).refresh);
	light.commit(f.query(light));
	CHECK(f.query(light).refresh);
	f.animated[0] = 0; // Turn off: one final static map.
	f.rebuild();
	CHECK(f.query(light).refresh);
	light.commit(f.query(light));
	CHECK_FALSE(f.query(light).refresh);
	// Selected opacity effectors use the same per-record animation path.
	f.rows[1].effect_params[1] = 1;
	float meta[4] = { 1, 0, 0, 0 };
	float spheres[GS_MAX_SPHERE_EFFECTORS][4] = {};
	float configs[GS_MAX_SPHERE_EFFECTORS][4] = {};
	float opacity[GS_MAX_SPHERE_EFFECTORS][4] = {};
	spheres[0][3] = 2; configs[0][0] = 1; opacity[0][1] = 1; opacity[0][2] = 0.5f;
	f.animated[1] = gs_shadow_refresh::effectors_active_on_casting_row(f.rows[1], true, meta, spheres, configs, opacity);
	f.rebuild();
	CHECK_FALSE(f.query(light).refresh);
	f.rows[0].effect_params[1] = 1;
	f.animated[0] = gs_shadow_refresh::effectors_active_on_casting_row(f.rows[0], true, meta, spheres, configs, opacity);
	f.rebuild();
	CHECK(f.query(light).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] inflated records enter light and final departure clears once") {
	RefreshFixture f;
	f.geometry[0].bounds.position.x = 4;
	f.publish();
	gs_shadow_refresh::LightState light;
	CHECK_FALSE(f.query(light).refresh); // Coarse world bounds alone must not observe this lamp.
	f.margins[0] = 2;
	f.rebuild();
	CHECK(f.query(light).refresh); // Inflated bound touches sphere.
	light.commit(f.query(light));
	f.margins[0] = 0;
	f.rebuild();
	CHECK(f.query(light).refresh);
	light.commit(f.query(light));
	CHECK_FALSE(f.query(light).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] wind forces intersecting casters while never-observing lights stay untouched") {
	LocalVector<gs_shadow_refresh::Owner> owners;
	gs_shadow_refresh::Owner owner;
	owner.owner = 42;
	owner.revision = 1;
	owner.bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	owners.push_back(owner);
	gs_shadow_refresh::LightState light;
	light.commit(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light));
	gs::settings::GSWindSettings wind;
	wind.enabled = true;
	wind.strength = 1;
	InstanceDataGPU row{};
	row.params[2] = 1;
	owners[0].animated = gs_wind::wind_active_on_casting_row(wind, row, true);
	CHECK(owners[0].animated);
	CHECK(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
	light.commit(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light));
	CHECK(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
	const gs_shadow_refresh::LightState never_observed;
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(20, 0, 0), 2, never_observed).refresh);
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(2, 2, 2), 1, never_observed).refresh);
	CHECK(gs_shadow_refresh::sphere_intersects_bounds(Vector3(3, 0, 0), 2, owner.bounds));
	CHECK(gs_shadow_refresh::positional_sphere_radius(10, -1) == 10);
	CHECK(gs_shadow_refresh::positional_sphere_radius(10, 60) >= real_t(19.99)); // Cone rim lies outside range sphere.
	owners[0].animated = false; // Turning deformation off needs one final static map.
	CHECK(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
	owners.clear(); // The collector supplies no entries on a non-HLOD route.
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, never_observed).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] a hidden previous owner clears once then reuses the empty map") {
	LocalVector<gs_shadow_refresh::Owner> owners;
	gs_shadow_refresh::Owner owner;
	owner.owner = 42;
	owner.revision = 1;
	owner.bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	owners.push_back(owner);
	gs_shadow_refresh::LightState light;
	light.commit(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light));
	owners.clear(); // Same collector outcome for hidden, freed, noncasting, non-HLOD, or released-renderer owners.
	const auto clear = gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light);
	CHECK(clear.refresh);
	CHECK(clear.owners.is_empty());
	// A refused scheduling attempt must retry; only the scheduled clearing pass consumes the departure.
	CHECK(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
	light.commit(clear);
	CHECK(light.last_pass.is_empty());
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] a previous owner leaving light range clears exactly once") {
	LocalVector<gs_shadow_refresh::Owner> owners;
	gs_shadow_refresh::Owner owner;
	owner.owner = 42;
	owner.revision = 1;
	owner.bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	owners.push_back(owner);
	gs_shadow_refresh::LightState light;
	light.commit(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light));
	owners[0].bounds.position = Vector3(20, -1, -1);
	const auto clear = gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light);
	CHECK(clear.refresh);
	CHECK(clear.owners.is_empty());
	light.commit(clear);
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] a light that never observed HLOD owners never requests a clearing pass") {
	LocalVector<gs_shadow_refresh::Owner> owners;
	gs_shadow_refresh::LightState light;
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
	gs_shadow_refresh::Owner owner;
	owner.owner = 42;
	owner.revision = 1;
	owner.animated = true;
	owner.bounds = AABB(Vector3(20, -1, -1), Vector3(2, 2, 2));
	owners.push_back(owner);
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
	owners.clear();
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] removing or replacing one of several observed owners clears once") {
	LocalVector<gs_shadow_refresh::Owner> owners;
	gs_shadow_refresh::Owner owner;
	owner.owner = 42;
	owner.revision = 1;
	owner.bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	owners.push_back(owner);
	owner.owner = 43;
	owners.push_back(owner);
	gs_shadow_refresh::LightState light;
	light.commit(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light));
	owners.resize(1);
	const auto removed = gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light);
	CHECK(removed.refresh);
	light.commit(removed);
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
	owners[0].owner = 44; // Replacement has the same set size but is unseen by this light.
	const auto replaced = gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light);
	CHECK(replaced.refresh);
	light.commit(replaced);
	CHECK_FALSE(gs_shadow_refresh::prepare_light(owners, Vector3(), 2, light).refresh);
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] static depth at two anchors has the same light clip coordinates") {
	Projection light;
	light.set_perspective(70, 1, 0.1, 100);
	const Vector3 world(2, 3, -10);
	const Vector3 anchors[2] = { Vector3(), Vector3(11, -7, 23) };
	const Vector4 expected = godot_shadow_clip(light, Transform3D(), false).xform(Vector4(world.x, world.y, world.z, 1));
	for (const Vector3 &anchor : anchors) {
		const Vector3 relative = world - anchor;
		const Vector4 clip = frame_relative_shadow_clip(light, Transform3D(), false, anchor).xform(Vector4(relative.x, relative.y, relative.z, 1));
		for (int axis = 0; axis < 4; ++axis) { CHECK(clip[axis] == doctest::Approx(expected[axis]).epsilon(1e-5)); }
	}
}

TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] selected sphere effectors refresh only responsive casting rows") {
	InstanceDataGPU row{};
	row.effect_params[0] = 1;
	float meta[4] = { 1, 0, 0, 1 };
	float spheres[GS_MAX_SPHERE_EFFECTORS][4] = {};
	float configs[GS_MAX_SPHERE_EFFECTORS][4] = {};
	float opacity[GS_MAX_SPHERE_EFFECTORS][4] = {};
	spheres[0][3] = 3;
	configs[0][0] = 1;
	configs[0][1] = 2;
	opacity[0][0] = 1;
	const uint32_t mask = 1;
	std::memcpy(&row.effect_params[2], &mask, sizeof(mask));
	CHECK(gs_shadow_refresh::effectors_active_on_casting_row(row, true, meta, spheres, configs, opacity));
	CHECK_FALSE(gs_shadow_refresh::effectors_active_on_casting_row(row, false, meta, spheres, configs, opacity));
	row.effect_params[2] = 0;
	CHECK_FALSE(gs_shadow_refresh::effectors_active_on_casting_row(row, true, meta, spheres, configs, opacity));
	meta[3] = 0; // Project fallback has no scene mask.
	CHECK(gs_shadow_refresh::effectors_active_on_casting_row(row, true, meta, spheres, configs, opacity));
	row.effect_params[0] = 0;
	CHECK_FALSE(gs_shadow_refresh::effectors_active_on_casting_row(row, true, meta, spheres, configs, opacity));
	row.effect_params[1] = 1;
	opacity[0][1] = 1;
	opacity[0][2] = 0.5f;
	CHECK(gs_shadow_refresh::effectors_active_on_casting_row(row, true, meta, spheres, configs, opacity));
	configs[0][0] = 0;
	CHECK_FALSE(gs_shadow_refresh::effectors_active_on_casting_row(row, true, meta, spheres, configs, opacity));
}

} // namespace TestHlodShadowCasters
