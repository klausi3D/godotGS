extern "C" int test_hlod_bundle_rows_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../renderer/hlod_bundle_rows.h"
#include "../renderer/hlod_contract_lifecycle.h"
#include "scene/3d/node_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

#include <cstring>

// PR1a of "world + movable instances in one frame": the HLOD bundle's instance-row region is multi-row. Today the route
// produces exactly one row (the world), so the N = 1 output must be the single-row bundle exactly; N > 1 is built by the
// same code and pinned here at unit level, before any caller produces it. CPU only.
namespace TestHlodBundleRows {
using namespace gs_hlod_bundle;

// The single-row ledger exactly as hlod_bundle_bytes() computed it before PR1a (base 992cb8eb733,
// renderer/resident_instance_contract_publisher.cpp:1206-1216), kept verbatim as the fixed reference.
static uint64_t single_row_reference_bytes(uint64_t p_nodes, uint64_t p_records, bool p_relative, bool p_quantized, uint32_t p_capacity) {
	return p_records * (p_quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian)) +
			p_nodes * (sizeof(ChunkMetaGPU) + sizeof(AssetChunkIndexGPU) + (p_relative ? sizeof(DrawnNodeGPU) : sizeof(VisibleChunkRefGPU)) +
					(p_quantized ? sizeof(ChunkQuantizationGPU) : 0)) +
			sizeof(AssetMetaGPU) + sizeof(InstanceDataGPU) + sizeof(InstanceGradingGPU) +
			uint64_t(p_capacity) * (sizeof(SplatRefGPU) + (p_relative ? sizeof(uint32_t) : 0)) +
			6 * sizeof(uint32_t) + 2 * sizeof(GaussianSplatting::IndirectDispatchLayout);
}

// The world identity row as GaussianSplatSceneDirector::build_instance_buffer_for_renderer writes it for a world
// submission (core/gaussian_splat_scene_director.cpp, world-submission branch), with fixed effector fields.
static InstanceDataGPU world_identity_row() {
	InstanceDataGPU entry = {};
	entry.rotation[3] = 1.0f;
	entry.inv_rotation[3] = 1.0f;
	entry.translation_scale[3] = 1.0f;
	entry.params[0] = 1.0f;
	entry.params[1] = 1.0f;
	entry.params[2] = 1.0f;
	entry.params[3] = 0.0f;
	entry.ids[0] = 0u;
	entry.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO |
			GS_INSTANCE_FLAG_ANTIALIASED;
	entry.wind_params[3] = 1.0f;
	entry.effect_params[0] = 1.0f;
	entry.effect_params[1] = 1.0f;
	entry.effect_params[3] = 2.0f;
	return entry;
}

static InstanceGradingGPU grading_row(float p_exposure) {
	InstanceGradingGPU g = {};
	g.primary[0] = 1.0f;
	g.primary[1] = p_exposure;
	g.primary[2] = 1.0f;
	g.primary[3] = 1.0f;
	return g;
}

// A distinct row per index: every field differs between rows, so a dropped, duplicated or reordered row is visible.
static InstanceDataGPU node_row(uint32_t p_index) {
	InstanceDataGPU entry = world_identity_row();
	const float f = float(p_index + 1);
	for (int c = 0; c < 4; ++c) {
		entry.rotation[c] = 0.125f * f + c;
		entry.inv_rotation[c] = -0.125f * f + c;
		entry.translation_scale[c] = 10.0f * f + c;
		entry.params[c] = 0.5f * f + c;
		entry.wind_params[c] = 0.25f * f + c;
		entry.effect_params[c] = 0.0625f * f + c;
	}
	entry.ids[0] = 0u;
	entry.ids[1] = 0x100u + p_index;
	entry.lod[0] = p_index;
	entry.lod[1] = 2u * p_index;
	return entry;
}

static bool rows_field_equal(const InstanceDataGPU &p_a, const InstanceDataGPU &p_b) {
	for (int c = 0; c < 4; ++c) {
		if (p_a.rotation[c] != p_b.rotation[c] || p_a.inv_rotation[c] != p_b.inv_rotation[c] ||
				p_a.translation_scale[c] != p_b.translation_scale[c] || p_a.params[c] != p_b.params[c] ||
				p_a.wind_params[c] != p_b.wind_params[c] || p_a.effect_params[c] != p_b.effect_params[c]) {
			return false;
		}
	}
	return p_a.ids[0] == p_b.ids[0] && p_a.ids[1] == p_b.ids[1] && p_a.lod[0] == p_b.lod[0] && p_a.lod[1] == p_b.lod[1];
}

TEST_CASE("[GaussianSplatting][HLOD] one-row bundle ledger equals the single-row ledger in every bundle shape") {
	const uint64_t node_counts[] = { 1u, 3u, 7u, 4096u, 1u << 20 };
	const uint64_t record_counts[] = { 1u, 2u, 1000u, 65536u, 1u << 24 };
	const uint32_t capacities[] = { 1u, 2u, 999u, 1u << 22 };
	uint32_t shapes = 0;
	for (uint64_t nodes : node_counts) {
		for (uint64_t records : record_counts) {
			for (uint32_t capacity : capacities) {
				for (bool relative : { false, true }) {
					for (bool quantized : { false, true }) {
						CHECK(bundle_bytes(nodes, records, relative, quantized, capacity, 1u) ==
								single_row_reference_bytes(nodes, records, relative, quantized, capacity));
						CHECK(visible_chunk_capacity(nodes, relative, 1u) == nodes);
						++shapes;
					}
				}
			}
		}
	}
	CHECK(shapes == 5u * 5u * 4u * 2u * 2u);
}

// #1466: a bundle buffer with no host payload (the streamed atlas, the visible/splat-ref/draw-entry lists, the counters and
// indirect headers) must not be created from a zero-filled host vector of its size. T0's streamed atlas is 5,259,264 records
// of 176 B; staged from the host it cost that size twice (the vector and RenderingDevice's transfer staging buffer).
TEST_CASE("[GaussianSplatting][HLOD] a bundle buffer without a host payload stages no host bytes at creation") {
	const uint64_t t0_atlas_bytes = 5259264ull * sizeof(PackedGaussian);
	CHECK(t0_atlas_bytes == 925630464ull);
	// No payload: nothing of the buffer's size may be allocated on the host, whatever the size.
	const uint64_t clearable[] = { 4u, 12u, sizeof(GaussianSplatting::IndirectDispatchLayout), sizeof(SplatRefGPU), sizeof(DrawnNodeGPU),
		sizeof(VisibleChunkRefGPU), sizeof(PackedGaussian), sizeof(PackedGaussianQuantized), t0_atlas_bytes, uint64_t(UINT32_MAX) - 3u };
	for (uint64_t bytes : clearable) {
		CHECK_MESSAGE(creation_host_staged_bytes(bytes, false) == 0u, "bytes=", bytes);
	}
	// A payload is staged in full: the legal route for the tables, the instance rows and a whole-tree atlas.
	for (uint64_t bytes : clearable) {
		CHECK_MESSAGE(creation_host_staged_bytes(bytes, true) == bytes, "bytes=", bytes);
	}
	// RD::buffer_clear takes a multiple of four bytes. A payload-free size that is not one keeps the host zero fill, so the
	// buffer's content is defined either way.
	for (uint64_t bytes : { uint64_t(1), uint64_t(2), uint64_t(3), uint64_t(13), uint64_t(UINT32_MAX) }) {
		CHECK_MESSAGE(creation_host_staged_bytes(bytes, false) == bytes, "bytes=", bytes);
	}
}

TEST_CASE("[GaussianSplatting][HLOD] one world row packs to exactly the director's row, field by field and byte by byte") {
	Rows rows;
	rows.instances.push_back(world_identity_row());
	rows.grading.push_back(grading_row(0.0f));
	rows.asset_ids.push_back(WORLD_ASSET_ID);
	CHECK(rows_refusal(rows, 7u) == nullptr);
	LocalVector<InstanceDataGPU> packed;
	pack_instance_rows(rows, packed);
	if (packed.size() != 1u) {
		FAIL("one row must pack to one row, got ", packed.size());
		return;
	}
	CHECK(rows_field_equal(packed[0], rows.instances[0]));
	CHECK(std::memcmp(packed.ptr(), rows.instances.ptr(), sizeof(InstanceDataGPU)) == 0);
	// The upload size of both row buffers is the old single-row size.
	CHECK(uint64_t(packed.size()) * sizeof(InstanceDataGPU) == sizeof(InstanceDataGPU));
	CHECK(uint64_t(rows.grading.size()) * sizeof(InstanceGradingGPU) == sizeof(InstanceGradingGPU));
	CHECK(dense_id_for_asset(WORLD_ASSET_ID) == WORLD_DENSE_ID);
	CHECK(WORLD_ROW == 0u);
}

TEST_CASE("[GaussianSplatting][HLOD] an N-row bundle packs every row in order and charges each row once") {
	const uint32_t n = 5u;
	Rows rows;
	for (uint32_t i = 0; i < n; ++i) {
		rows.instances.push_back(i == 0 ? world_identity_row() : node_row(i));
		rows.grading.push_back(grading_row(0.25f * i));
		rows.asset_ids.push_back(WORLD_ASSET_ID);
	}
	// A row whose ids[0] still carries a director-side value is rewritten to the dense id of its asset.
	rows.instances[3].ids[0] = 77u;
	CHECK(rows_refusal(rows, 9u) == nullptr);
	LocalVector<InstanceDataGPU> packed;
	pack_instance_rows(rows, packed);
	if (packed.size() != n) {
		FAIL("expected ", n, " packed rows, got ", packed.size());
		return;
	}
	for (uint32_t i = 0; i < n; ++i) {
		InstanceDataGPU expected = rows.instances[i];
		expected.ids[0] = WORLD_DENSE_ID;
		CHECK_MESSAGE(rows_field_equal(packed[i], expected), "row ", i, " differs");
		CHECK_MESSAGE(std::memcmp(&packed[i], &expected, sizeof(InstanceDataGPU)) == 0, "row ", i, " bytes differ");
	}
	CHECK(packed[3].ids[0] == WORLD_DENSE_ID);
	CHECK(rows_field_equal(packed[0], world_identity_row())); // The world stays row 0.
	for (uint32_t i = 1; i < n; ++i) {
		CHECK_FALSE(rows_field_equal(packed[i], packed[i - 1])); // Distinct fixtures: a duplicated row would pass otherwise.
	}
	// Ledger: every extra row adds one instance and one grading row; the GPU-culled canonical bundle also gets a
	// visible-chunk slot per (row, node). The relative draw list stays node-indexed.
	const uint64_t nodes = 9u, records = 1000u;
	const uint32_t capacity = 600u;
	for (bool quantized : { false, true }) {
		const uint64_t row_bytes = sizeof(InstanceDataGPU) + sizeof(InstanceGradingGPU);
		CHECK(bundle_bytes(nodes, records, true, quantized, capacity, n) ==
				bundle_bytes(nodes, records, true, quantized, capacity, 1u) + (n - 1u) * row_bytes);
		CHECK(bundle_bytes(nodes, records, false, quantized, capacity, n) ==
				bundle_bytes(nodes, records, false, quantized, capacity, 1u) + (n - 1u) * (row_bytes + nodes * sizeof(VisibleChunkRefGPU)));
	}
	CHECK(visible_chunk_capacity(nodes, false, n) == nodes * n);
	CHECK(visible_chunk_capacity(nodes, true, n) == nodes);
}

TEST_CASE("[GaussianSplatting][HLOD] bundle rows that cannot be packed are refused with a reason") {
	Rows empty;
	CHECK(String(rows_refusal(empty, 3u)) == "hlod_bundle_rows_empty");

	Rows mismatched_grading;
	mismatched_grading.instances.push_back(world_identity_row());
	mismatched_grading.instances.push_back(node_row(1));
	mismatched_grading.grading.push_back(grading_row(0.0f));
	mismatched_grading.asset_ids.push_back(WORLD_ASSET_ID);
	mismatched_grading.asset_ids.push_back(WORLD_ASSET_ID);
	CHECK(String(rows_refusal(mismatched_grading, 3u)) == "hlod_bundle_rows_mismatched");

	Rows mismatched_ids;
	mismatched_ids.instances.push_back(world_identity_row());
	mismatched_ids.grading.push_back(grading_row(0.0f));
	CHECK(String(rows_refusal(mismatched_ids, 3u)) == "hlod_bundle_rows_mismatched");

	// PR1a packs only the world: a row naming a node asset is refused, never drawn with the world's payload.
	Rows foreign_asset;
	foreign_asset.instances.push_back(world_identity_row());
	foreign_asset.instances.push_back(node_row(1));
	foreign_asset.grading.push_back(grading_row(0.0f));
	foreign_asset.grading.push_back(grading_row(0.0f));
	foreign_asset.asset_ids.push_back(WORLD_ASSET_ID);
	foreign_asset.asset_ids.push_back(42u);
	CHECK(String(rows_refusal(foreign_asset, 3u)) == "hlod_bundle_row_asset_not_packed");
	CHECK(dense_id_for_asset(42u) == UINT32_MAX);

	// Two rows over a 2^31-node tree need a 2^32-slot canonical visible list: more than one 32-bit buffer holds.
	Rows two;
	two.instances.push_back(world_identity_row());
	two.instances.push_back(node_row(1));
	two.grading.push_back(grading_row(0.0f));
	two.grading.push_back(grading_row(0.0f));
	two.asset_ids.push_back(WORLD_ASSET_ID);
	two.asset_ids.push_back(WORLD_ASSET_ID);
	CHECK(String(rows_refusal(two, uint64_t(1u) << 31)) == "hlod_bundle_rows_exceed_capacity");
	CHECK(rows_refusal(two, 1024u) == nullptr);
}

TEST_CASE("[GaussianSplatting][HLOD] object-only asset changes fitting the fixed tail preserve the world (#1473)") {
	ContractChange change;
	change.prepared_asset_generation = 4;
	change.asset_generation = 5;
	change.prepared_row_generation = change.row_generation = 9; // Asset generation ONLY.
	change.reserved_object_pages = 10;
	for (uint64_t pages : { 0u, 9u, 10u }) {
		CAPTURE(pages);
		change.object_pages = pages;
		const auto decision = hlod_contract_change_action(change);
		CHECK(decision.reason == ContractChangeReason::ASSETS_CHANGED);
		CHECK_MESSAGE(decision.action == ContractChangeAction::INSTANCE_REGION_ONLY,
				"#1473: an object tail that fits must not tear down streamed-world residency");
	}
}

TEST_CASE("[GaussianSplatting][HLOD] lifecycle replacement overflow and unchanged controls (#1473)") {
	ContractChange change;
	change.prepared_asset_generation = change.asset_generation = 4;
	change.prepared_row_generation = change.row_generation = 9;
	CHECK(hlod_contract_change_action(change).action == ContractChangeAction::ROWS_ONLY);
	CHECK(hlod_contract_change_action(change).reason == ContractChangeReason::UNCHANGED);
	++change.row_generation;
	CHECK(hlod_contract_change_action(change).action == ContractChangeAction::ROWS_ONLY);
	CHECK(hlod_contract_change_action(change).reason == ContractChangeReason::ROWS_CHANGED);
	++change.asset_generation;
	change.reserved_object_pages = 10;
	change.object_pages = 11;
	CHECK(hlod_contract_change_action(change).action == ContractChangeAction::FULL_REBUILD);
	CHECK(hlod_contract_change_action(change).reason == ContractChangeReason::OBJECT_TAIL_OVERFLOW);
	change.object_pages = 1;
	change.world_device_source_current = false; // World, device or source replacement invalidates this identity.
	CHECK(hlod_contract_change_action(change).action == ContractChangeAction::FULL_REBUILD);
	CHECK(hlod_contract_change_action(change).reason == ContractChangeReason::WORLD_DEVICE_SOURCE_CHANGED);
	change.world_device_source_current = true;
	change.resources_current = false;
	CHECK(hlod_contract_change_action(change).action == ContractChangeAction::FULL_REBUILD);
	CHECK(hlod_contract_change_action(change).reason == ContractChangeReason::RESOURCES_CHANGED);
}

TEST_CASE("[GaussianSplatting][HLOD] a prepared bundle rebuilds on an asset-set change and re-uploads on a row change") {
	// Unchanged.
	CHECK(prepared_rows_action(4, 4, 9, 9, 1, 1) == PreparedRows::CURRENT);
	// Row content only (transform, params, grading ref): re-upload in place.
	CHECK(prepared_rows_action(4, 4, 9, 10, 3, 3) == PreparedRows::REUPLOAD);
	// Row content and row count (a node became hidden): the bundle is rebuilt.
	CHECK(prepared_rows_action(4, 4, 9, 10, 3, 2) == PreparedRows::REBUILD);
	// Asset set: always a rebuild, whatever the rows did.
	CHECK(prepared_rows_action(4, 5, 9, 9, 1, 1) == PreparedRows::REBUILD);
	CHECK(prepared_rows_action(4, 5, 9, 10, 1, 1) == PreparedRows::REBUILD);
	// The row count is only read when the row generation moved.
	CHECK(prepared_rows_action(4, 4, 9, 9, 3, 0) == PreparedRows::CURRENT);
}

static Ref<GaussianSplatAsset> one_splat_asset() {
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	asset->set_splat_count(1);
	PackedFloat32Array positions;
	positions.push_back(0.0f);
	positions.push_back(0.0f);
	positions.push_back(0.0f);
	asset->set_positions(positions);
	PackedFloat32Array scales;
	scales.push_back(1.0f);
	scales.push_back(1.0f);
	scales.push_back(1.0f);
	asset->set_scales(scales);
	PackedFloat32Array rotations;
	rotations.push_back(1.0f);
	rotations.push_back(0.0f);
	rotations.push_back(0.0f);
	rotations.push_back(0.0f);
	asset->set_rotations(rotations);
	PackedFloat32Array dc;
	dc.push_back(1.0f);
	dc.push_back(1.0f);
	dc.push_back(1.0f);
	asset->set_sh_dc_coefficients(dc);
	PackedFloat32Array opacity;
	opacity.push_back(10.0f);
	asset->set_opacity_logits(opacity);
	return asset;
}

// The two director counters the prepared HLOD bundle is keyed on, driven through the public director API: a movement or
// a parameter edit moves only the row generation (re-upload); adding or removing an instance moves the asset-set
// generation too (rebuild).
TEST_CASE("[GaussianSplatting][HLOD][SceneDirector][SceneTree] node movement re-uploads rows, instance set changes rebuild the bundle") {
	SceneTree *tree = SceneTree::get_singleton();
	if (tree == nullptr || tree->get_root() == nullptr) {
		FAIL("SceneTree root must exist (provided by [SceneTree] tag)");
		return;
	}
	Window *root = tree->get_root();
	Ref<World3D> world = root->get_world_3d();
	if (!world.is_valid() || !world->get_scenario().is_valid()) {
		FAIL("SceneTree root must have a valid World3D scenario");
		return;
	}
	const RID scenario = world->get_scenario();
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = director == nullptr;
	if (owns_director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	if (director == nullptr) {
		FAIL("GaussianSplatSceneDirector must be available");
		return;
	}
	Node3D *node_a = memnew(Node3D);
	Node3D *node_b = memnew(Node3D);
	root->add_child(node_a);
	root->add_child(node_b);
	tree->process(0.0);
	Ref<GaussianSplatAsset> asset = one_splat_asset();

	director->register_instance(node_a->get_instance_id(), asset, Transform3D(), 1.0f, 0.0f, 0u);
	uint64_t asset_gen = director->test_instance_asset_generation_for_scenario(scenario);
	uint64_t row_gen = director->test_instance_generation_for_scenario(scenario);
	CHECK(asset_gen > 0);
	CHECK(row_gen > 0);
	auto step = [&](const char *p_what, PreparedRows p_expected) {
		const uint64_t next_asset = director->test_instance_asset_generation_for_scenario(scenario);
		const uint64_t next_row = director->test_instance_generation_for_scenario(scenario);
		CHECK_MESSAGE(next_row > row_gen, p_what, ": the row generation must advance");
		CHECK_MESSAGE(prepared_rows_action(asset_gen, next_asset, row_gen, next_row, 1u, 1u) == p_expected, p_what);
		asset_gen = next_asset;
		row_gen = next_row;
	};

	Transform3D moved;
	moved.origin = Vector3(5.0f, 0.0f, -3.0f);
	director->update_instance_transform(node_a->get_instance_id(), moved);
	step("transform-only update", PreparedRows::REUPLOAD);

	director->update_instance_params(node_a->get_instance_id(), 0.5f, 0.0f, 0u);
	step("parameter update", PreparedRows::REUPLOAD);

	director->register_instance(node_b->get_instance_id(), asset, Transform3D(), 1.0f, 0.0f, 0u);
	step("adding an instance", PreparedRows::REBUILD);

	director->unregister_instance(node_b->get_instance_id());
	step("removing an instance", PreparedRows::REBUILD);

	director->unregister_instance(node_a->get_instance_id());
	root->remove_child(node_b);
	root->remove_child(node_a);
	memdelete(node_b);
	memdelete(node_a);
	tree->process(0.0);
	if (owns_director) {
		memdelete(director);
	}
}

} // namespace TestHlodBundleRows
