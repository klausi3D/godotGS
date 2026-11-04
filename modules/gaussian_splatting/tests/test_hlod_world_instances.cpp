extern "C" int test_hlod_world_instances_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../renderer/hlod_world_instances.h"
#include "core/math/random_pcg.h"
#include "scene/3d/node_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/world_3d.h"

#include <cmath>
#include <cstring>

// PR1b of "world + movable instances in one frame" (docs/architecture/adr-one-route-many-producers.md): the HLOD route
// draws the world (row 0) and the visible GaussianSplatNode3D instances (rows 1..N) in one draw list, one depth sort and
// one byte ledger, under the budget rule of ADR 4.5. CPU only: the director row builder and route predicate through the
// public director API, and the device-free bundle pieces of renderer/hlod_world_instances.h that the publisher runs.
namespace TestHlodWorldInstances {
using namespace gs_hlod_bundle;

#define GS_NEED(cond) \
	do { \
		if (!(cond)) { \
			FAIL("requirement failed: ", #cond); \
			return; \
		} \
	} while (0)

static Ref<GaussianSplatAsset> blob_asset(uint32_t p_splats) {
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	asset->set_splat_count(p_splats);
	PackedFloat32Array positions, scales, rotations, dc, opacity;
	for (uint32_t i = 0; i < p_splats; ++i) {
		positions.push_back(0.01f * float(i));
		positions.push_back(0.0f);
		positions.push_back(0.0f);
		for (int a = 0; a < 3; ++a) { scales.push_back(0.05f); }
		rotations.push_back(1.0f);
		rotations.push_back(0.0f);
		rotations.push_back(0.0f);
		rotations.push_back(0.0f);
		for (int a = 0; a < 3; ++a) { dc.push_back(1.0f); }
		opacity.push_back(10.0f);
	}
	asset->set_positions(positions);
	asset->set_scales(scales);
	asset->set_rotations(rotations);
	asset->set_sh_dc_coefficients(dc);
	asset->set_opacity_logits(opacity);
	return asset;
}

static Ref<GaussianData> world_data(int p_count) {
	Ref<GaussianData> data;
	data.instantiate();
	data->resize(p_count);
	for (int i = 0; i < p_count; ++i) {
		Gaussian g = {};
		g.position = Vector3(float(i), 0.0f, -10.0f);
		g.scale = Vector3(0.5f, 0.5f, 0.5f);
		g.rotation = Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
		g.opacity = 1.0f;
		g.sh_dc = Color(1.0f, 0.0f, 0.0f, 1.0f);
		data->set_gaussian(i, g);
	}
	return data;
}

// A director scenario with a world submission (owner node) and nodes registered as instances; no RenderingDevice, so no
// renderer is created and only the director's bookkeeping runs (which is what the HLOD rows are built from).
struct DirectorScene {
	SceneTree *tree = nullptr;
	Window *root = nullptr;
	RID scenario;
	GaussianSplatSceneDirector *director = nullptr;
	bool owns_director = false;
	Node *owner = nullptr;
	LocalVector<Node3D *> nodes;

	bool init() {
		tree = SceneTree::get_singleton();
		root = tree ? tree->get_root() : nullptr;
		if (!root || root->get_world_3d().is_null() || !root->get_world_3d()->get_scenario().is_valid()) { return false; }
		scenario = root->get_world_3d()->get_scenario();
		director = GaussianSplatSceneDirector::get_singleton();
		if (!director) {
			director = memnew(GaussianSplatSceneDirector);
			owns_director = true;
		}
		owner = memnew(Node);
		root->add_child(owner);
		tree->process(0.0);
		GaussianSplatSceneDirector::WorldSubmission submission;
		submission.owner_id = owner->get_instance_id();
		submission.scenario = scenario;
		submission.gaussian_data = world_data(16);
		return director->submit_world_submission(submission);
	}
	Node3D *add_node() {
		Node3D *node = memnew(Node3D);
		root->add_child(node);
		nodes.push_back(node);
		return node;
	}
	~DirectorScene() {
		if (director) {
			for (Node3D *node : nodes) { director->unregister_instance(node->get_instance_id()); }
			if (owner) { director->release_world_submission(owner->get_instance_id()); }
		}
		for (Node3D *node : nodes) {
			root->remove_child(node);
			memdelete(node);
		}
		if (owner) {
			root->remove_child(owner);
			memdelete(owner);
		}
		if (tree) { tree->process(0.0); }
		if (owns_director) { memdelete(director); }
	}
	void rows(Rows &r_rows, LocalVector<uint64_t> &r_keys) const {
		director->test_build_hlod_instance_rows_for_scenario(scenario, r_rows.instances, r_rows.grading, r_rows.asset_ids, r_keys);
	}
	uint64_t owner_key() const { return uint64_t(owner->get_instance_id()); }
};

// A region that packs the given assets the way hlod_pack_instance_assets() lays them out (ascending ids, whole chunks of
// `p_chunk_splats`), then tallied against `p_rows`.
static InstanceRegion region_for(const LocalVector<uint64_t> &p_asset_ids, const LocalVector<uint64_t> &p_splats, uint32_t p_chunk_splats,
		const Rows *p_rows = nullptr) {
	InstanceRegion region;
	LocalVector<uint32_t> order;
	for (uint32_t i = 0; i < p_asset_ids.size(); ++i) { order.push_back(i); }
	std::sort(order.ptr(), order.ptr() + order.size(), [&](uint32_t a, uint32_t b) { return p_asset_ids[a] < p_asset_ids[b]; });
	for (uint32_t i : order) {
		InstanceAsset asset;
		asset.asset_id = p_asset_ids[i];
		asset.first_chunk = region.chunk_count;
		asset.first_record = region.records;
		asset.splat_count = p_splats[i];
		asset.chunk_count = uint32_t((p_splats[i] + p_chunk_splats - 1) / p_chunk_splats);
		asset.max_chunk_splats = uint32_t(MIN<uint64_t>(p_splats[i], p_chunk_splats));
		asset.bounds_radius = 0.5f;
		asset.footprint = 36.0 * 0.05 * 0.05 * double(p_splats[i]);
		region.chunk_count += asset.chunk_count;
		region.records += asset.splat_count;
		region.max_chunk_splats = MAX(region.max_chunk_splats, asset.max_chunk_splats);
		region.assets.push_back(asset);
	}
	if (p_rows) { tally_rows(*p_rows, region); }
	return region;
}

TEST_CASE("[GaussianSplatting][HLOD][SceneDirector][SceneTree] a world with 0, 1 and 8 node instances keeps the HLOD route and its row 0; nodes are rows 1..N") {
	DirectorScene scene;
	GS_NEED(scene.init());
	Ref<GaussianSplatAsset> asset_a = blob_asset(3);
	Ref<GaussianSplatAsset> asset_b = blob_asset(5);
	const uint64_t id_a = uint64_t(asset_a->get_instance_id());
	const uint64_t id_b = uint64_t(asset_b->get_instance_id());
	for (uint32_t k : { 0u, 1u, 8u }) {
		while (scene.nodes.size() < k) {
			const uint32_t i = scene.nodes.size();
			Node3D *node = scene.add_node();
			Transform3D t;
			t.origin = Vector3(float(i) - 3.5f, 0.25f * float(i), -6.0f - 2.0f * float(i % 2));
			// Node 5 is hidden: a hidden record must not remove the world (the canonical builder's is_empty() rule did).
			scene.director->register_instance(node->get_instance_id(), (i % 3) == 1 ? asset_b : asset_a, t, 1.0f, 0.0f, 0u, false,
					1.0f, GaussianSplatSceneDirector::INSTANCE_WIND_INHERIT, Vector3(), 1.0f, /*visible=*/i != 5);
		}
		// Routing: the world submission is the HLOD route's world whatever the node count (no exclusivity).
		CHECK_MESSAGE(scene.director->test_has_hlod_world_submission_for_scenario(scene.scenario, scene.owner_key()), "k=", k);
		CHECK_FALSE(scene.director->test_has_hlod_world_submission_for_scenario(scene.scenario, scene.owner_key() + 1));
		Rows rows;
		LocalVector<uint64_t> keys;
		scene.rows(rows, keys);
		const uint32_t visible = k > 5 ? k - 1 : k;
		if (rows.count() != visible + 1 || keys.size() != rows.count() || rows.grading.size() != rows.count() || rows.asset_ids.size() != rows.count()) {
			FAIL("k=", k, ": expected ", visible + 1, " parallel rows, got ", rows.count(), "/", keys.size(), "/", rows.grading.size(), "/", rows.asset_ids.size());
			return;
		}
		// Row 0 is the world identity row, asset 0, key 0.
		CHECK(rows.asset_ids[0] == WORLD_ASSET_ID);
		CHECK(keys[0] == 0u);
		CHECK(rows.instances[0].ids[0] == 0u);
		CHECK((rows.instances[0].ids[1] & (GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO)) ==
				(GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO));
		CHECK(rows.instances[0].translation_scale[0] == 0.0f);
		CHECK(rows.instances[0].translation_scale[3] == 1.0f);
		// Rows 1..N: the visible nodes, in registration order, with their transform, 64-bit asset id and stable key.
		uint32_t row = 1;
		for (uint32_t i = 0; i < k; ++i) {
			if (i == 5) { continue; }
			if (row >= rows.count()) { FAIL("missing row for node ", i); return; }
			CHECK(keys[row] == uint64_t(scene.nodes[i]->get_instance_id()));
			CHECK(rows.asset_ids[row] == ((i % 3) == 1 ? id_b : id_a));
			CHECK(rows.instances[row].translation_scale[0] == doctest::Approx(float(i) - 3.5f));
			CHECK(rows.instances[row].translation_scale[2] == doctest::Approx(-6.0f - 2.0f * float(i % 2)));
			CHECK((rows.instances[row].ids[1] & GS_INSTANCE_FLAG_TRANSLATION_ZERO) == 0u);
			++row;
		}
		// No fallback reason: the rows pack into one bundle with the node assets, row 0 the world.
		LocalVector<uint64_t> assets;
		LocalVector<uint64_t> splats;
		if (k > 0) { assets.push_back(id_a); splats.push_back(3); }
		if (k > 1) { assets.push_back(id_b); splats.push_back(5); }
		const InstanceRegion region = region_for(assets, splats, 4096u, &rows);
		const char *reason = rows_refusal(rows, 31u, region);
		CHECK_MESSAGE(reason == nullptr, "k=", k, " reason=", reason ? reason : "");
		// Without the region (the PR1a world-only bundle) any node row is refused, never drawn with the world's payload.
		if (k > 0) { CHECK(String(rows_refusal(rows, 31u, InstanceRegion())) == "hlod_bundle_row_asset_not_packed"); }
	}
	// The route ends with the world submission, not with the nodes.
	scene.director->release_world_submission(scene.owner->get_instance_id());
	CHECK_FALSE(scene.director->test_has_hlod_world_submission_for_scenario(scene.scenario, scene.owner_key()));
	Rows after;
	LocalVector<uint64_t> after_keys;
	scene.rows(after, after_keys);
	CHECK(after.count() == 0u);
}

TEST_CASE("[GaussianSplatting][HLOD][SceneDirector][SceneTree] moving world-scene nodes re-uploads the rows in place; a hidden or added node rebuilds") {
	DirectorScene scene;
	GS_NEED(scene.init());
	Ref<GaussianSplatAsset> asset = blob_asset(4);
	for (uint32_t i = 0; i < 8; ++i) {
		Node3D *node = scene.add_node();
		Transform3D t;
		t.origin = Vector3(float(i), 0.0f, -5.0f);
		scene.director->register_instance(node->get_instance_id(), asset, t, 1.0f, 0.0f, 0u);
	}
	Rows prepared;
	LocalVector<uint64_t> keys;
	scene.rows(prepared, keys);
	GS_NEED(prepared.count() == 9u);
	LocalVector<uint64_t> ids;
	ids.push_back(uint64_t(asset->get_instance_id()));
	LocalVector<uint64_t> splats;
	splats.push_back(4);
	const InstanceRegion region = region_for(ids, splats, 4096u, &prepared);
	GS_NEED(rows_refusal(prepared, 7u, region) == nullptr);
	CHECK(region.row_chunks == 8u); // Two instances of one asset share its payload: 8 rows, 1 packed asset.
	CHECK(region.records == 4u);
	uint64_t asset_gen = scene.director->test_instance_asset_generation_for_scenario(scene.scenario);
	uint64_t row_gen = scene.director->test_instance_generation_for_scenario(scene.scenario);
	// Every node moves (a game frame): the row generation moves, the asset set does not, so the prepared bundle is kept
	// and its rows are re-uploaded; the new rows still pack against the same region (same assets per row).
	for (uint32_t frame = 1; frame <= 3; ++frame) {
		for (uint32_t i = 0; i < 8; ++i) {
			Transform3D t;
			t.origin = Vector3(float(i), 0.1f * float(frame), -5.0f - float(frame));
			scene.director->update_instance_transform(scene.nodes[i]->get_instance_id(), t);
		}
		const uint64_t next_asset = scene.director->test_instance_asset_generation_for_scenario(scene.scenario);
		const uint64_t next_row = scene.director->test_instance_generation_for_scenario(scene.scenario);
		Rows moved;
		LocalVector<uint64_t> moved_keys;
		scene.rows(moved, moved_keys);
		CHECK(next_asset == asset_gen);
		CHECK(next_row > row_gen);
		CHECK(prepared_rows_action(asset_gen, next_asset, row_gen, next_row, prepared.count(), moved.count()) == PreparedRows::REUPLOAD);
		CHECK(rows_refusal(moved, 7u, region) == nullptr);
		if (moved.count() == 9u) {
			CHECK(moved.instances[3].translation_scale[2] == doctest::Approx(-5.0f - float(frame)));
			CHECK(moved_keys[3] == keys[3]);
		}
		row_gen = next_row;
	}
	// Hiding a node changes the asset selection (and the row count): rebuild.
	scene.director->update_instance_params(scene.nodes[2]->get_instance_id(), 1.0f, 0.0f, 0u, false, 1.0f, GaussianSplatSceneDirector::INSTANCE_WIND_INHERIT,
			Vector3(), 1.0f, /*visible=*/false);
	Rows hidden;
	LocalVector<uint64_t> hidden_keys;
	scene.rows(hidden, hidden_keys);
	CHECK(hidden.count() == 8u);
	CHECK(prepared_rows_action(asset_gen, scene.director->test_instance_asset_generation_for_scenario(scene.scenario), row_gen,
				  scene.director->test_instance_generation_for_scenario(scene.scenario), prepared.count(), hidden.count()) == PreparedRows::REBUILD);
	// A row set that no longer matches the packed region's tallies is refused, never re-uploaded into the old layout.
	CHECK(String(rows_refusal(hidden, 7u, region)) == "hlod_bundle_rows_region_mismatch");
}

TEST_CASE("[GaussianSplatting][HLOD] the world-plus-instances ledger charges each node asset once and is PR1a's ledger without instances") {
	// Empty region: exactly PR1a's ledger and capacities, in every bundle shape.
	const uint64_t node_counts[] = { 1u, 7u, 4096u };
	const uint64_t record_counts[] = { 1u, 1000u, 1u << 20 };
	const uint32_t capacities[] = { 1u, 999u, 1u << 22 };
	for (uint64_t nodes : node_counts) {
		for (uint64_t records : record_counts) {
			for (uint32_t capacity : capacities) {
				for (bool relative : { false, true }) {
					for (bool quantized : { false, true }) {
						for (uint32_t rows : { 1u, 3u }) {
							InstanceRegion empty;
							empty.world_rows = rows;
							CHECK(bundle_bytes(nodes, records, relative, quantized, capacity, rows, empty) ==
									bundle_bytes(nodes, records, relative, quantized, capacity, rows));
							CHECK(visible_chunk_capacity(nodes, relative, empty) == visible_chunk_capacity(nodes, relative, rows));
						}
					}
				}
			}
		}
	}
	for (uint32_t leaf : { 1u, 20000u, 6000000u }) {
		for (int max_splats : { 0, 1, 15000, 5000000 }) {
			// Whatever the view cap C (it is only the objects' pool), an empty region gives PR1a's capacity.
			for (uint64_t view : { uint64_t(0), uint64_t(1), uint64_t(20000), uint64_t(50000000) }) {
				CHECK(relative_capacity(leaf, max_splats, view, InstanceRegion()) == MIN<uint32_t>(leaf, uint32_t(MAX(1, max_splats))));
			}
			CHECK(canonical_capacity(leaf, InstanceRegion()) == leaf);
		}
	}
	// Two assets (A: 5000 splats in 2 chunks, B: 100 splats in 1 chunk); three rows of A and one of B after the world.
	Rows rows;
	for (uint64_t asset : { uint64_t(0), uint64_t(900), uint64_t(77), uint64_t(900), uint64_t(900) }) {
		InstanceDataGPU r = {};
		rows.instances.push_back(r);
		rows.grading.push_back(InstanceGradingGPU{});
		rows.asset_ids.push_back(asset);
	}
	LocalVector<uint64_t> ids;
	ids.push_back(900);
	ids.push_back(77);
	LocalVector<uint64_t> splats;
	splats.push_back(5000);
	splats.push_back(100);
	const InstanceRegion region = region_for(ids, splats, 4096u, &rows);
	GS_NEED(region.assets.size() == 2u);
	CHECK(region.assets[0].asset_id == 77u); // Ascending ids; dense id = 1 + index.
	CHECK(dense_id_for_asset(77u, region) == 1u);
	CHECK(dense_id_for_asset(900u, region) == 2u);
	CHECK(dense_id_for_asset(5u, region) == UINT32_MAX);
	CHECK(region.chunk_count == 3u);
	CHECK(region.records == 5100u); // Each asset's payload once, however many rows name it.
	CHECK(region.row_chunks == 3u * 2u + 1u);
	CHECK(region.row_splats == 3u * 5000u + 100u);
	CHECK(region.world_rows == 1u);
	CHECK(rows_refusal(rows, 31u, region) == nullptr);
	LocalVector<InstanceDataGPU> packed;
	pack_instance_rows(rows, region, packed);
	GS_NEED(packed.size() == 5u);
	CHECK(packed[0].ids[0] == 0u);
	CHECK(packed[1].ids[0] == 2u);
	CHECK(packed[2].ids[0] == 1u);
	const uint64_t nodes = 31u, world_records = 20000u;
	const uint32_t capacity = 40000u;
	for (bool relative : { false, true }) {
		for (bool quantized : { false, true }) {
			const uint64_t stride = quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
			const uint64_t tables = sizeof(ChunkMetaGPU) + sizeof(AssetChunkIndexGPU) + (quantized ? sizeof(ChunkQuantizationGPU) : 0);
			const uint64_t slot = relative ? sizeof(DrawnNodeGPU) : sizeof(VisibleChunkRefGPU);
			const uint64_t expected = bundle_bytes(nodes, world_records, relative, quantized, capacity, 5u) // PR1a, 5 world-style rows
					- (relative ? 0u : 4u * nodes * slot) // PR1a gave every row a canonical slot per node; node rows use their asset's chunks.
					+ region.records * stride + region.chunk_count * tables + region.row_chunks * slot + 2u * sizeof(AssetMetaGPU);
			CHECK(bundle_bytes(nodes, world_records, relative, quantized, capacity, 5u, region) == expected);
		}
	}
	CHECK(visible_chunk_capacity(nodes, true, region) == nodes + 7u);
	CHECK(visible_chunk_capacity(nodes, false, region) == nodes + 7u);
	// The streamed page budget: everything but the world's atlas records is fixed bytes, so the instance region (atlas tail
	// included) shrinks the page budget by exactly its own bytes.
	for (bool quantized : { false, true }) {
		const uint64_t stride = quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
		InstanceRegion world_only;
		world_only.world_rows = 1;
		const uint64_t fixed_with = bundle_bytes(nodes, 1u, true, quantized, capacity, 5u, region) - stride;
		const uint64_t fixed_without = bundle_bytes(nodes, 1u, true, quantized, capacity, 5u, world_only) - stride;
		const uint64_t tables = sizeof(ChunkMetaGPU) + sizeof(AssetChunkIndexGPU) + (quantized ? sizeof(ChunkQuantizationGPU) : 0);
		CHECK(fixed_with - fixed_without == region.records * stride + region.chunk_count * tables + region.row_chunks * sizeof(DrawnNodeGPU) +
						2u * sizeof(AssetMetaGPU));
	}
	// Capacities: the relative bundle holds the world's PR1a capacity plus what the rows can be admitted to (at most the
	// object share), never more than S; the canonical bundle every leaf and every row's asset.
	CHECK(relative_capacity(20000u, 5000000, 5000000u, region) == 20000u + 15100u);
	CHECK(relative_capacity(20000u, 30000, 30000u, region) == 30000u);
	CHECK(relative_capacity(4000u, 30000, 30000u, region) == 4000u + 15000u); // object share of 30000 is 15000
	// A world clamped to its own data count (S = 20000 = leaf count) in a view of C = 1,000,000: room for every row.
	CHECK(relative_capacity(20000u, 20000, 1000000u, region) == 20000u + 15100u);
	CHECK(canonical_capacity(20000u, region) == 35100u);
	// Capacity refusals: a draw list that does not fit a 32-bit buffer.
	CHECK(String(rows_refusal(rows, uint64_t(1u) << 30, region)) == "hlod_bundle_rows_exceed_capacity");
	Rows not_world_first = rows;
	not_world_first.asset_ids[0] = 77u;
	CHECK(String(rows_refusal(not_world_first, 31u, region)) == "hlod_world_row_unavailable");
}

static ObjectDemand demand(uint64_t p_key, uint32_t p_row, uint64_t p_splats, double p_records, double p_importance, uint64_t p_chunks = 1) {
	ObjectDemand d;
	d.instance_key = p_key;
	d.row = p_row;
	d.splats = p_splats;
	d.chunks = p_chunks;
	d.records = p_records;
	d.importance = p_importance;
	return d;
}

TEST_CASE("[GaussianSplatting][HLOD] the budget rule admits objects in importance order with stable ties, refuses the rest whole and keeps the world floor") {
	// The floor F is one half of each performance cap.
	CHECK(WORLD_FLOOR_NUMERATOR * 2u == WORLD_FLOOR_DENOMINATOR);
	CHECK(world_floor_splats(1000u) == 500u);
	CHECK(object_share_splats(1000u) == 500u);
	CHECK(world_floor_splats(1001u) == 501u);
	CHECK(object_share_splats(1001u) == 500u);
	CHECK(world_floor_splats(1u) == 1u);
	CHECK(object_share_splats(1u) == 0u);
	CHECK(object_share_records(800.0) == doctest::Approx(400.0));
	CHECK(String(INSTANCE_REFUSAL_REASON) == "hlod_instance_reserve_exceeds_object_share");

	// Eight objects of 150 splats against a splat cap of 1000: the object share is 500, so exactly three fit. Importance
	// decides which; equal importance falls back to the stable instance key, never to the input order.
	LocalVector<ObjectDemand> objects;
	objects.push_back(demand(808, 1, 150, 10.0, 4.0));
	objects.push_back(demand(101, 2, 150, 10.0, 9.0));
	objects.push_back(demand(505, 3, 150, 10.0, 4.0));
	objects.push_back(demand(303, 4, 150, 10.0, 4.0));
	objects.push_back(demand(202, 5, 150, 10.0, 1.0));
	objects.push_back(demand(707, 6, 150, 10.0, 4.0));
	objects.push_back(demand(606, 7, 150, 10.0, NAN)); // Sorts as importance 0.
	objects.push_back(demand(404, 8, 150, 10.0, 2.0));
	ObjectShare share;
	share.splats = object_share_splats(1000u);
	share.nodes = 1000u;
	share.records = object_share_records(1.0e6);
	Admission reference;
	admit_objects(objects.ptr(), objects.size(), share, reference);
	GS_NEED(reference.admitted.size() == 3u);
	GS_NEED(reference.refused.size() == 5u);
	// Admitted: 101 (importance 9), then the two smallest keys among importance 4: 303 and 505. Reported in row order
	// (rows 2, 3, 4).
	CHECK(objects[reference.admitted[0]].instance_key == 101u);
	CHECK(objects[reference.admitted[1]].instance_key == 505u);
	CHECK(objects[reference.admitted[2]].instance_key == 303u);
	CHECK(objects[reference.admitted[0]].row < objects[reference.admitted[1]].row);
	CHECK(reference.splats == 450u);
	// Refused in admission order: 707, 808 (importance 4), 404 (2), 202 (1), 606 (NaN -> 0).
	const uint64_t refused_keys[5] = { 707, 808, 404, 202, 606 };
	for (uint32_t i = 0; i < 5; ++i) { CHECK(objects[reference.refused[i]].instance_key == refused_keys[i]); }
	// Determinism: every permutation of the input admits the same set (the same view always admits the same objects).
	RandomPCG rng(1131u);
	for (int trial = 0; trial < 64; ++trial) {
		LocalVector<ObjectDemand> shuffled = objects;
		for (uint32_t i = shuffled.size() - 1; i > 0; --i) {
			const uint32_t j = rng.rand() % (i + 1);
			SWAP(shuffled[i], shuffled[j]);
		}
		Admission again;
		admit_objects(shuffled.ptr(), shuffled.size(), share, again);
		GS_NEED(again.admitted.size() == 3u);
		LocalVector<uint64_t> keys;
		for (uint32_t index : again.admitted) { keys.push_back(shuffled[index].instance_key); }
		CHECK(keys[0] == 101u);
		CHECK(keys[1] == 505u);
		CHECK(keys[2] == 303u);
	}

	// A large object that does not fit is refused whole, and the smaller, less important objects behind it are still
	// admitted: objects are never dropped as a group because one does not fit.
	LocalVector<ObjectDemand> mixed;
	mixed.push_back(demand(1, 1, 400, 5.0, 10.0));
	mixed.push_back(demand(2, 2, 200, 5.0, 8.0)); // 400 + 200 > 500: refused.
	mixed.push_back(demand(3, 3, 90, 5.0, 1.0)); // Still fits.
	mixed.push_back(demand(4, 4, 20, 5.0, 0.5)); // 490 + 20 > 500: refused.
	Admission partial;
	admit_objects(mixed.ptr(), mixed.size(), share, partial);
	GS_NEED(partial.admitted.size() == 2u);
	CHECK(mixed[partial.admitted[0]].instance_key == 1u);
	CHECK(mixed[partial.admitted[1]].instance_key == 3u);
	GS_NEED(partial.refused.size() == 2u);
	CHECK(mixed[partial.refused[0]].instance_key == 2u);
	CHECK(mixed[partial.refused[1]].instance_key == 4u);
	CHECK(partial.splats == 490u);

	// Every cap binds: the record share and the draw-slot share refuse too.
	ObjectShare tight = share;
	tight.records = 12.0;
	Admission by_records;
	admit_objects(mixed.ptr(), mixed.size(), tight, by_records);
	CHECK(by_records.admitted.size() == 2u); // 5 + 5 <= 12, a third 5 does not fit.
	CHECK(by_records.records <= tight.records);
	ObjectShare one_slot = share;
	one_slot.nodes = 1u;
	Admission by_nodes;
	admit_objects(mixed.ptr(), mixed.size(), one_slot, by_nodes);
	CHECK(by_nodes.admitted.size() == 1u);
	// A non-finite record estimate never admits.
	LocalVector<ObjectDemand> broken;
	broken.push_back(demand(9, 1, 1, NAN, 1.0));
	Admission none;
	admit_objects(broken.ptr(), broken.size(), share, none);
	CHECK(none.admitted.size() == 0u);
	CHECK(none.refused.size() == 1u);

	// The world floor: whatever the objects take, the world cut keeps at least F of each cap; without objects it keeps the
	// PR1a caps exactly.
	for (uint32_t leaf : { 300u, 2000u, 9000000u }) {
		for (uint64_t cap : { uint64_t(1), uint64_t(1000), uint64_t(5000000) }) {
			for (bool streaming : { false, true }) {
				uint64_t splats = 0;
				double records = 0.0;
				world_view_caps(leaf, int(cap), cap, streaming, 700u, 1.0e5, Admission(), splats, records);
				const uint64_t pr1a = streaming ? MIN<uint64_t>(MIN<uint64_t>(leaf, cap), 700u) : MIN<uint64_t>(leaf, cap);
				CHECK(splats == pr1a);
				CHECK(records == 1.0e5);
				LocalVector<ObjectDemand> greedy;
				for (uint32_t i = 0; i < 40; ++i) { greedy.push_back(demand(i + 1, i + 1, cap / 7 + 1, 1.0e4, double(i))); }
				ObjectShare s;
				s.splats = object_share_splats(cap);
				s.nodes = 1000u;
				s.records = object_share_records(1.0e5);
				Admission taken;
				admit_objects(greedy.ptr(), greedy.size(), s, taken);
				CHECK(taken.splats <= object_share_splats(cap));
				CHECK(taken.records <= object_share_records(1.0e5));
				world_view_caps(leaf, int(cap), cap, streaming, 700u, 1.0e5, taken, splats, records);
				CHECK(splats >= MIN<uint64_t>(MIN<uint64_t>(leaf, streaming ? 700u : leaf), world_floor_splats(cap)));
				CHECK(records >= world_floor_records(1.0e5));
				CHECK(taken.refused.size() > 0u); // 40 objects of more than cap/7 each never all fit half the cap.
			}
		}
	}
}

// Coordinator follow-up 1 (ADR 4.5): the objects reserve from the VIEW cap C, not from the world's own splat count. A
// resident world's contract clamps max_splats to its data count; that bounds what the world draws, it is not the pool.
TEST_CASE("[GaussianSplatting][HLOD] world plus objects that fit under the view cap leave the world cut exactly as without objects") {
	const uint32_t leaf = 25600u;
	const int world_max_splats = 25600; // Clamped to the world's data count.
	const uint64_t view_cap = 1000000u; // The view's performance cap (pre-clamp).
	uint64_t alone = 0, with_objects = 0;
	double alone_records = 0.0, with_records = 0.0;
	world_view_caps(leaf, world_max_splats, view_cap, false, 0u, 2.0e5, Admission(), alone, alone_records);
	CHECK(alone == 25600u);
	LocalVector<ObjectDemand> objects;
	for (uint32_t i = 0; i < 8; ++i) { objects.push_back(demand(100 + i, i + 1, 1500, 8000.0, double(8 - i))); }
	ObjectShare share;
	share.splats = object_share_splats(view_cap);
	share.nodes = 8u;
	// The overlap storage was grown from the world's 2e5 to 2e5 + 64000 for the objects (ensure_hlod_overlap_capacity).
	share.records = object_share_records(double(UINT32_MAX), 2.64e5, 2.0e5);
	Admission admission;
	admit_objects(objects.ptr(), objects.size(), share, admission);
	CHECK(admission.admitted.size() == 8u);
	CHECK(admission.refused.size() == 0u);
	world_view_caps(leaf, world_max_splats, view_cap, false, 0u, 2.64e5, admission, with_objects, with_records);
	CHECK(with_objects == alone); // Nothing degraded: W + R <= C.
	CHECK(with_records == doctest::Approx(alone_records));
	// The bundle holds the total: world need plus every admitted object.
	InstanceRegion region;
	region.row_splats = 8u * 1500u;
	CHECK(relative_capacity(leaf, world_max_splats, view_cap, region) == 25600u + 12000u);
	// Control: with the clamped S as the objects' pool (the PR1b-draft behaviour) the objects came out of the world.
	uint64_t clamped_pool = 0;
	double ignored = 0.0;
	world_view_caps(leaf, world_max_splats, uint64_t(world_max_splats), false, 0u, 2.64e5, admission, clamped_pool, ignored);
	CHECK(clamped_pool == 25600u - 12000u);
}

TEST_CASE("[GaussianSplatting][HLOD] world plus objects over the view cap: the world keeps the floor and objects are refused in order") {
	const uint32_t leaf = 25600u;
	const uint64_t view_cap = 9010u; // max_splat_count lowered for the view; the world needs 25600.
	LocalVector<ObjectDemand> objects;
	for (uint32_t i = 0; i < 8; ++i) { objects.push_back(demand(200 + i, i + 1, 1500, 10.0, double(8 - i))); }
	ObjectShare share;
	share.splats = object_share_splats(view_cap); // 4505
	share.nodes = 8u;
	share.records = 1.0e9;
	Admission admission;
	admit_objects(objects.ptr(), objects.size(), share, admission);
	GS_NEED(admission.admitted.size() == 3u);
	GS_NEED(admission.refused.size() == 5u);
	for (uint32_t i = 0; i < 3; ++i) { CHECK(objects[admission.admitted[i]].instance_key == 200u + i); } // Most important first.
	for (uint32_t i = 0; i < 5; ++i) { CHECK(objects[admission.refused[i]].instance_key == 203u + i); }
	uint64_t world = 0;
	double records = 0.0;
	world_view_caps(leaf, int(view_cap), view_cap, false, 0u, 1.0e6, admission, world, records);
	CHECK(world == view_cap - 4500u);
	CHECK(world >= world_floor_splats(view_cap));
	// The record share never hands the objects what the storage holds for the world's floor.
	CHECK(object_share_records(1.0e9, 1.0e5, 1.0e5) == doctest::Approx(5.0e4));
	CHECK(object_share_records(1.0e9, 1.6e5, 1.0e5) == doctest::Approx(8.0e4)); // The world keeps min(need, storage / 2).
	CHECK(object_share_records(2.0e5, 1.0e6, 1.0e5) == doctest::Approx(5.0e5)); // The view cap is at least the storage.
	CHECK(object_share_records(3.0e6, 1.0e6, 9.0e5) == doctest::Approx(5.0e5)); // A big world need keeps half.
}

TEST_CASE("[GaussianSplatting][HLOD][SceneDirector] a resident world's contract clamps max_splats to its data count but keeps the view cap") {
	GaussianSplatSceneDirector::WorldSubmission submission;
	submission.gaussian_data = world_data(16);
	submission.desired_renderer_overrides[StringName("max_splats")] = int64_t(500000);
	GaussianSplatRenderer::WorldSubmissionContract contract = GaussianSplatSceneDirector::test_build_world_contract(submission, 1000000);
	CHECK(contract.max_splats == 16); // What the world can draw.
	CHECK(contract.view_max_splats == 500000); // The pool the node instances reserve from.
	// No override: the renderer's baseline is the view cap.
	GaussianSplatSceneDirector::WorldSubmission plain;
	plain.gaussian_data = world_data(16);
	contract = GaussianSplatSceneDirector::test_build_world_contract(plain, 300000);
	CHECK(contract.max_splats == 16);
	CHECK(contract.view_max_splats == 300000);
	// A world larger than the cap: both are the cap.
	GaussianSplatSceneDirector::WorldSubmission big;
	big.gaussian_data = world_data(64);
	big.desired_renderer_overrides[StringName("max_splats")] = int64_t(40);
	contract = GaussianSplatSceneDirector::test_build_world_contract(big, 1000000);
	CHECK(contract.max_splats == 40);
	CHECK(contract.view_max_splats == 40);
}

TEST_CASE("[GaussianSplatting][HLOD] an object's view demand: outside the frustum it reserves nothing; inside it is priced like a world node") {
	InstanceAsset asset;
	asset.asset_id = 5;
	asset.splat_count = 200;
	asset.chunk_count = 1;
	asset.footprint = 2.0;
	asset.bounds_radius = 0.5f;
	Projection projection;
	projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);
	Vector<Plane> planes = projection.get_projection_planes(Transform3D());
	GS_NEED(planes.size() == 6);
	Plane frustum[6];
	for (int i = 0; i < 6; ++i) { frustum[i] = planes[i]; }
	double importance = -1.0, records = -1.0;
	CHECK_FALSE(object_view_demand(asset, Vector3(0, 0, 10), 1.0f, frustum, true, Vector3(), 400.0, 256.0, importance, records)); // Behind.
	CHECK(object_view_demand(asset, Vector3(0, 0, 10), 1.0f, frustum, false, Vector3(), 400.0, 256.0, importance, records)); // Frustum off.
	GS_NEED(object_view_demand(asset, Vector3(0, 0, -10), 1.0f, frustum, true, Vector3(), 400.0, 256.0, importance, records));
	CHECK(importance == doctest::Approx(0.5 * 400.0 / 10.0));
	CHECK(records == doctest::Approx(200.0 + 2.0 * 400.0 * 400.0 / (9.5 * 9.5 * 256.0)));
	double near_importance = 0.0, near_records = 0.0;
	GS_NEED(object_view_demand(asset, Vector3(0, 0, -2), 1.0f, frustum, true, Vector3(), 400.0, 256.0, near_importance, near_records));
	CHECK(near_importance > importance); // Closer is more important and costs more records.
	CHECK(near_records > records);
	// Scale enlarges the object (radius and footprint); inside the sphere the distance is floored like the selector's.
	double scaled_importance = 0.0, scaled_records = 0.0;
	GS_NEED(object_view_demand(asset, Vector3(0, 0, -10), 2.0f, frustum, true, Vector3(), 400.0, 256.0, scaled_importance, scaled_records));
	CHECK(scaled_importance == doctest::Approx(2.0 * importance));
	double inside_importance = 0.0, inside_records = 0.0;
	GS_NEED(object_view_demand(asset, Vector3(0, 0, -0.2f), 1.0f, frustum, false, Vector3(), 400.0, 256.0, inside_importance, inside_records));
	const double floored = gs_hlod_runtime::kCostInsideDistanceFraction * 0.5;
	CHECK(inside_records == doctest::Approx(200.0 + 2.0 * 400.0 * 400.0 / (floored * floored * 256.0)));
	CHECK(std::isfinite(inside_records));
}

TEST_CASE("[GaussianSplatting][HLOD] instance draw records name their row and chunk after the world, with T - camera in double at 1 km and 100 km") {
	LocalVector<uint64_t> ids;
	ids.push_back(10);
	ids.push_back(20);
	LocalVector<uint64_t> splats;
	splats.push_back(9000); // 3 chunks of 4096
	splats.push_back(10); // 1 chunk
	const InstanceRegion region = region_for(ids, splats, 4096u);
	GS_NEED(region.assets.size() == 2u);
	LocalVector<DrawnNodeGPU> draws;
	append_instance_draws(region, 31u, 4u, 0u, Vector3(1, 2, 3), draws);
	append_instance_draws(region, 31u, 2u, 1u, Vector3(-4, 5, -6), draws);
	GS_NEED(draws.size() == 4u);
	for (uint32_t c = 0; c < 3; ++c) {
		CHECK(draws[c].instance_id == 4u);
		CHECK(draws[c].chunk_id == 31u + c);
		CHECK(draws[c].camera_offset[0] == 1.0f);
		CHECK(draws[c].fade == 1.0f);
	}
	CHECK(draws[3].instance_id == 2u);
	CHECK(draws[3].chunk_id == 31u + 3u); // Asset 20 follows asset 10 in the region.
	CHECK(draws[3].camera_offset[2] == -6.0f);
	// T - camera: a node 1 km and 100 km from the origin, the camera a few metres from it. The offset is formed in double
	// from the float T and the precise camera, so it is exact to float rounding of the small result.
	for (double distance : { 1000.0, 100000.0 }) {
		const Vector3 t(float(distance) + 0.25f, 3.5f, -float(distance) - 0.5f);
		const double camera_abs[3] = { double(t.x) - 1.2345, double(t.y) - 2.0, double(t.z) + 3.75 };
		gs_world_frame::PreciseCoord camera;
		GS_NEED(gs_world_frame::PreciseCoord::from_doubles(camera_abs, camera));
		Vector3 offset;
		GS_NEED(instance_camera_offset(t, gs_world_frame::PreciseCoord(), camera, offset));
		CHECK(std::fabs(double(offset.x) - 1.2345) < 1.0e-6);
		CHECK(offset.y == doctest::Approx(2.0).epsilon(1e-6));
		CHECK(offset.z == doctest::Approx(-3.75).epsilon(1e-6));
		// A float subtraction of the absolute positions loses the offset at 100 km (float spacing there is ~8 mm): the
		// control shows the precise path is what keeps it.
		const float naive = float(t.x) - float(camera_abs[0]);
		if (distance > 50000.0) { CHECK(std::fabs(double(naive) - 1.2345) > 1.0e-5); }
	}
}

} // namespace TestHlodWorldInstances
