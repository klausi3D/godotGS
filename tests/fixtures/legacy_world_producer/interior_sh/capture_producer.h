#pragma once

// Evidence-only successor helper. The production baker, writer, reader and ABI
// remain identical to the historical producer. Preserve its original six files.
#include "test_legacy_world_producer_capture.h"
#include "modules/gaussian_splatting/core/gaussian_splat_hlod_bake.h"
#include "modules/gaussian_splatting/core/streaming_chunk_payload_source.h"
#include "core/io/json.h"

namespace TestLegacyInteriorShCapture {
static Array vector_values(const Vector3 &p_value) {
	Array values;
	values.push_back(p_value.x);
	values.push_back(p_value.y);
	values.push_back(p_value.z);
	return values;
}

static Dictionary gaussian_values(const Gaussian &p_g) {
	Dictionary values;
	values["position"] = vector_values(p_g.position);
	values["opacity"] = p_g.opacity;
	values["scale"] = vector_values(p_g.scale);
	values["area"] = p_g.area;
	Array rotation;
	rotation.push_back(p_g.rotation.x);
	rotation.push_back(p_g.rotation.y);
	rotation.push_back(p_g.rotation.z);
	rotation.push_back(p_g.rotation.w);
	values["rotation"] = rotation;
	Array dc;
	dc.push_back(p_g.sh_dc.r);
	dc.push_back(p_g.sh_dc.g);
	dc.push_back(p_g.sh_dc.b);
	dc.push_back(p_g.sh_dc.a);
	values["sh_dc"] = dc;
	Array first;
	for (int i = 0; i < 3; i++) {
		first.push_back(vector_values(p_g.sh_1[i]));
	}
	values["sh_first"] = first;
	values["normal"] = vector_values(p_g.normal);
	values["stroke_age"] = p_g.stroke_age;
	values["padding"] = p_g._padding;
	Array axes;
	axes.push_back(p_g.brush_axes.x);
	axes.push_back(p_g.brush_axes.y);
	values["brush_axes"] = axes;
	values["painterly_meta"] = int64_t(p_g.painterly_meta);
	values["render_meta"] = int64_t(p_g.render_meta);
	Array padding2;
	padding2.push_back(p_g._padding2[0]);
	padding2.push_back(p_g._padding2[1]);
	values["padding2"] = padding2;
	return values;
}

static void check_gaussian(const Gaussian &p_got, const Gaussian &p_expected) {
	CHECK(p_got.position.is_equal_approx(p_expected.position));
	CHECK_EQ(p_got.opacity, p_expected.opacity);
	CHECK(p_got.scale.is_equal_approx(p_expected.scale));
	CHECK_EQ(p_got.area, p_expected.area);
	CHECK(p_got.rotation.is_equal_approx(p_expected.rotation));
	CHECK(p_got.sh_dc.is_equal_approx(p_expected.sh_dc));
	for (int i = 0; i < 3; i++) {
		CHECK(p_got.sh_1[i].is_equal_approx(p_expected.sh_1[i]));
	}
	CHECK(p_got.normal.is_equal_approx(p_expected.normal));
	CHECK_EQ(p_got.stroke_age, p_expected.stroke_age);
	CHECK_EQ(p_got._padding, p_expected._padding);
	CHECK(p_got.brush_axes.is_equal_approx(p_expected.brush_axes));
	CHECK_EQ(p_got.painterly_meta, p_expected.painterly_meta);
	CHECK_EQ(p_got.render_meta, p_expected.render_meta);
	CHECK_EQ(p_got._padding2[0], p_expected._padding2[0]);
	CHECK_EQ(p_got._padding2[1], p_expected._padding2[1]);
}

static Dictionary node_values(const GaussianSplatHlodTree &p_tree, const GaussianSplatHlodNode &p_node) {
	Dictionary values;
	values["e"] = p_node.cell.e;
	values["ix"] = p_node.cell.ix;
	values["iy"] = p_node.cell.iy;
	values["iz"] = p_node.cell.iz;
	values["aabb_min"] = vector_values(p_node.aabb_min);
	values["aabb_max"] = vector_values(p_node.aabb_max);
	values["radius"] = p_node.radius;
	values["geometric_error"] = p_node.geometric_error;
	values["error_chosen"] = p_node.error_chosen;
	values["error_rejected"] = p_node.error_rejected;
	values["parent"] = int64_t(p_node.parent);
	values["first_child"] = int64_t(p_node.first_child);
	values["child_count"] = int64_t(p_node.child_count);
	values["height"] = int64_t(p_node.height);
	values["payload_first"] = int64_t(p_node.payload_first);
	values["payload_count"] = int64_t(p_node.payload_count);
	values["flags"] = int64_t(p_node.flags);
	values["overlap_footprint"] = p_node.overlap_footprint;
	double center[3];
	p_tree.node_cell_center(p_node, center);
	Array frame;
	for (int i = 0; i < 3; i++) {
		frame.push_back(center[i]);
	}
	values["cell_center"] = frame;
	return values;
}

static bool check_world(const Ref<GaussianSplatWorld> &p_world, const gs_hlod::BakeResult &p_expected, bool p_staged) {
	if (p_world.is_null() || !p_world->has_hlod_tree()) {
		FAIL("actual historical reader must publish the captured HLOD tree");
		return false;
	}
	const GaussianSplatHlodTree &got = p_world->get_hlod_tree();
	const GaussianSplatHlodTree &expected = p_expected.tree;
	CHECK_EQ(got.leaf_splat_count, expected.leaf_splat_count);
	CHECK_EQ(got.interior_splat_count, expected.interior_splat_count);
	CHECK_EQ(got.sh_high_order_count, 5u);
	CHECK_EQ(got.bake_rule_version, expected.bake_rule_version);
	CHECK_EQ(got.top_level_nodes.size(), 0u);
	CHECK_EQ(got.instances.size(), 0u);
	for (int axis = 0; axis < 3; axis++) {
		CHECK_EQ(got.origin[axis], expected.origin[axis]);
	}
	String reason;
	CHECK(gs_hlod_validate_tree(got, &reason));
	if (got.nodes.size() != expected.nodes.size()) {
		FAIL("historical reader changed the actual bake node cardinality");
		return false;
	}
	for (uint32_t i = 0; i < got.nodes.size(); i++) {
		CHECK(node_values(got, got.nodes[i]) == node_values(expected, expected.nodes[i]));
	}
	LocalVector<Gaussian> interior;
	LocalVector<Vector3> interior_sh;
	if (got.read_interior_payload(interior, interior_sh) != OK ||
			interior.size() != expected.interior_gaussians.size() ||
			interior_sh.size() != expected.interior_sh_high_order.size()) {
		FAIL("historical reader must decode the actual baked interior payload and SH");
		return false;
	}
	for (uint32_t i = 0; i < interior.size(); i++) {
		check_gaussian(interior[i], expected.interior_gaussians[i]);
	}
	for (uint32_t i = 0; i < interior_sh.size(); i++) {
		CHECK(interior_sh[i].is_equal_approx(expected.interior_sh_high_order[i]));
	}
	LocalVector<Gaussian> leaves;
	LocalVector<Vector3> leaf_sh;
	uint32_t first = 0u, high = 0u;
	CHECK_EQ(p_world->has_chunk_payload_source(), p_staged);
	if (p_staged) {
		Ref<StagedFileChunkPayloadSource> source = p_world->get_chunk_payload_source();
		if (source.is_null() || !source->is_valid() ||
				!source->capture_chunk_snapshot(0u, 2u, leaves, leaf_sh, first, high)) {
			FAIL("historical staged reader must decode frame-correct leaves and SH");
			return false;
		}
	} else {
		Ref<GaussianData> data = p_world->get_gaussian_data();
		if (data.is_null() || !data->capture_chunk_snapshot(0u, 2u, leaves, leaf_sh, first, high)) {
			FAIL("historical resident reader must decode frame-correct leaves and SH");
			return false;
		}
	}
	CHECK_EQ(first, 3u);
	CHECK_EQ(high, 5u);
	if (leaves.size() != p_expected.leaf_gaussians.size() || leaf_sh.size() != p_expected.leaf_sh_high_order.size()) {
		FAIL("historical reader changed actual baked leaf/SH cardinality");
		return false;
	}
	for (uint32_t i = 0; i < leaves.size(); i++) {
		check_gaussian(leaves[i], p_expected.leaf_gaussians[i]);
	}
	for (uint32_t i = 0; i < leaf_sh.size(); i++) {
		CHECK(leaf_sh[i].is_equal_approx(p_expected.leaf_sh_high_order[i]));
	}
	if (p_staged) {
		Ref<StagedFileChunkPayloadSource> source = p_world->get_chunk_payload_source();
		const uint32_t reverse[] = {1u, 0u};
		if (!source->capture_indexed_chunk_snapshot(reverse, 2u, leaves, leaf_sh, first, high) ||
				leaves.size() != 2u || leaf_sh.size() != 10u) {
			FAIL("historical indexed capture must preserve leaf identity, frames and SH");
			return false;
		}
		CHECK_EQ(first, 3u);
		CHECK_EQ(high, 5u);
		CHECK_EQ(source->get_file_open_count(), 1u);
		for (uint32_t i = 0; i < 2u; i++) {
			check_gaussian(leaves[i], p_expected.leaf_gaussians[reverse[i]]);
			for (uint32_t k = 0; k < 5u; k++) {
				CHECK(leaf_sh[i * 5u + k].is_equal_approx(p_expected.leaf_sh_high_order[reverse[i] * 5u + k]));
			}
		}
	}
	return true;
}
} // namespace TestLegacyInteriorShCapture

TEST_CASE("[LegacyInteriorShCapture] actual old branching HLOD v2 and SH producer fixtures") {
	using namespace TestLegacyInteriorShCapture;
	String directory = OS::get_singleton()->get_environment("GS_LEGACY_INTERIOR_SH_CAPTURE_DIR").replace("\\", "/");
	if ((!directory.begins_with("Z:/") && !directory.begins_with("C:/")) ||
			DirAccess::make_dir_recursive_absolute(directory) != OK ||
			FileAccess::exists(directory.path_join("bake-result.json"))) {
		FAIL("a fresh authorized C:/Z: extension output directory is required");
		return;
	}
	LocalVector<Gaussian> input;
	LocalVector<Vector3> input_sh;
	for (int i = 0; i < 2; i++) {
		Gaussian g;
		g.position = Vector3(4.25f + 8.0f * i, 5.5f, 6.75f);
		g.opacity = 0.75f;
		g.scale = Vector3(0.25f, 0.5f, 0.75f);
		g.rotation = Quaternion();
		g.sh_dc = Color(0.25f, 0.5f, 0.75f, 1.0f);
		g.normal = Vector3(0.0f, 1.0f, 0.0f);
		g.brush_axes = Vector2(1.0f, 1.0f);
		g.painterly_meta = gaussian_pack_painterly_meta(uint16_t(41 + i), 5);
		for (int k = 0; k < 3; k++) {
			const float value = float(1 + i * 3 + k) / 64.0f;
			g.sh_1[k] = Vector3(value, -value, value * 2.0f);
		}
		input.push_back(g);
		for (int k = 0; k < 5; k++) {
			const float value = float(1 + i * 5 + k) / 128.0f;
			input_sh.push_back(Vector3(value, value * 2.0f, -value));
		}
	}
	gs_hlod::BakeInput bake_input;
	bake_input.gaussians = input.ptr();
	bake_input.count = input.size();
	bake_input.sh_high_order = input_sh.ptr();
	bake_input.sh_high_order_count = 5u;
	gs_hlod::BakeParams params;
	params.leaf_max_splats = 1u;
	gs_hlod::BakeResult baked;
	String reason;
	if (!gs_hlod::bake_world(bake_input, params, baked, &reason) || baked.tree.nodes.size() <= 1u ||
			baked.tree.interior_splat_count == 0u || baked.tree.interior_gaussians.is_empty()) {
		FAIL("unchanged old baker must genuinely emit branching nodes and interior payload: ", reason.utf8().get_data());
		return;
	}
	CHECK(gs_hlod_validate_tree(baked.tree, &reason));
	if (baked.leaf_source_index.size() != 2u || baked.leaf_gaussians.size() != 2u || baked.leaf_sh_high_order.size() != 10u) {
		FAIL("actual bake must retain both leaves and their five higher-order vectors");
		return;
	}
	for (uint32_t i = 0; i < 2u; i++) {
		const uint32_t source = baked.leaf_source_index[i];
		if (source >= 2u) {
			FAIL("actual bake source permutation must reference an authored leaf");
			return;
		}
		check_gaussian(baked.leaf_gaussians[i], input[source]);
		for (uint32_t k = 0; k < 5u; k++) {
			CHECK(baked.leaf_sh_high_order[i * 5u + k].is_equal_approx(input_sh[source * 5u + k]));
		}
	}
	CHECK_NE(baked.leaf_source_index[0], baked.leaf_source_index[1]);
	Ref<GaussianData> data;
	data.instantiate();
	data->set_gaussian_payload(baked.leaf_gaussians, baked.leaf_sh_high_order, 3u, 5u, false);
	CHECK_EQ(data->get_sh_first_order_count(), 3u);
	CHECK_EQ(data->get_sh_high_order_count(), 5u);
	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_gaussian_data(data);
	AABB bounds;
	if (!gs_hlod_bounds_enclosing_root(baked.tree, data->get_aabb(), bounds)) {
		FAIL("actual baked root must fit the historical world bounds");
		return;
	}
	world->set_bounds(bounds);
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	if (!GaussianSplatWorld::build_hlod_leaf_chunks(baked.tree, chunks)) {
		FAIL("historical chunk producer must accept the actual baked leaves");
		return;
	}
	world->set_static_chunks(chunks);
	GaussianSplatHlodTree installed = baked.tree;
	world->set_hlod_tree(std::move(installed));
	Dictionary metadata;
	metadata["fixture"] = "actual old branching HLOD baker with first-order3 and higher-order5";
	world->set_metadata(metadata);
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (int mode = 0; mode < 3; mode++) {
		const String suffix = mode == 0 ? "streamable" : mode == 1 ? "resident" : "compressed";
		const String path = directory.path_join("hlod_v2_interior_sh_" + suffix + ".gsplatworld");
		if (FileAccess::exists(path)) {
			FAIL("extension outputs must never overwrite an existing capture");
			return;
		}
		const Error saved = mode == 0 ? saver.save(world, path)
				: mode == 1 ? saver.save_resident_uncompressed(world, path) : saver.save_resident_compressed(world, path);
		if (saved != OK) {
			FAIL("actual historical saver rejected branching SH world");
			return;
		}
		{
			Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
			if (file.is_null() || file->get_length() < 184u) {
				FAIL("actual historical saver must emit a complete v2 header");
				return;
			}
			file->seek(4u);
			CHECK_EQ(file->get_32(), 2u);
			const uint32_t flags = file->get_32();
			CHECK_NE(flags & (1u << 6u), 0u);
			CHECK_EQ((flags & (1u << 4u)) != 0u, mode == 2);
			CHECK_EQ((flags & (1u << 5u)) != 0u, mode != 0);
			file->seek(20u); // degree is at16; first/high counts are at20/24.
			CHECK_EQ(file->get_32(), 3u);
			CHECK_EQ(file->get_32(), 5u);
			file->seek(56u);
			CHECK_EQ(file->get_64(), 184u);
			file->seek(132u);
			CHECK_EQ(file->get_32(), baked.tree.interior_splat_count);
			file->get_64(); // Actual node-table offset, authenticated from bytes externally.
			CHECK_EQ(file->get_32(), baked.tree.nodes.size());
		}
		Error err = ERR_BUG;
		Ref<GaussianSplatWorld> resident = loader.load_resident(path, &err);
		if (err != OK || !check_world(resident, baked, false)) {
			FAIL("historical resident reader failed its own authentic producer output");
			return;
		}
		CHECK(resident->get_metadata() == metadata);
		err = ERR_BUG;
		Ref<GaussianSplatWorld> normal = loader.load(path, "", &err);
		if (err != OK || !check_world(normal, baked, mode == 0)) {
			FAIL("historical normal reader failed its own authentic producer output");
			return;
		}
		CHECK(normal->get_metadata() == metadata);
	}
	// These expected values are produced from BakeResult AFTER the native checks;
	// no header, node, frame or positive fixture bytes are constructed by the helper.
	Dictionary expected;
	expected["leaf_count"] = int64_t(baked.tree.leaf_splat_count);
	expected["interior_count"] = int64_t(baked.tree.interior_splat_count);
	expected["first_order_count"] = 3;
	expected["higher_order_count"] = 5;
	expected["bake_rule_version"] = int64_t(baked.tree.bake_rule_version);
	expected["metadata"] = metadata;
	Array origin;
	for (int i = 0; i < 3; i++) {
		origin.push_back(baked.tree.origin[i]);
	}
	expected["origin"] = origin;
	Array nodes, leaves, interiors, leaf_sh, interior_sh, permutation;
	for (uint32_t i = 0; i < baked.tree.nodes.size(); i++) {
		nodes.push_back(node_values(baked.tree, baked.tree.nodes[i]));
	}
	for (uint32_t i = 0; i < baked.leaf_gaussians.size(); i++) {
		leaves.push_back(gaussian_values(baked.leaf_gaussians[i]));
		permutation.push_back(int64_t(baked.leaf_source_index[i]));
	}
	for (uint32_t i = 0; i < baked.tree.interior_gaussians.size(); i++) {
		interiors.push_back(gaussian_values(baked.tree.interior_gaussians[i]));
	}
	for (uint32_t i = 0; i < baked.leaf_sh_high_order.size(); i++) {
		leaf_sh.push_back(vector_values(baked.leaf_sh_high_order[i]));
	}
	for (uint32_t i = 0; i < baked.tree.interior_sh_high_order.size(); i++) {
		interior_sh.push_back(vector_values(baked.tree.interior_sh_high_order[i]));
	}
	expected["nodes"] = nodes;
	expected["leaf_gaussians"] = leaves;
	expected["interior_gaussians"] = interiors;
	expected["leaf_sh_high_order"] = leaf_sh;
	expected["interior_sh_high_order"] = interior_sh;
	expected["leaf_source_index"] = permutation;
	expected["top_level_count"] = 0;
	expected["instance_count"] = 0;
	Ref<FileAccess> result = FileAccess::open(directory.path_join("bake-result.json"), FileAccess::WRITE);
	if (result.is_null()) {
		FAIL("actual BakeResult evidence file must be writable");
		return;
	}
	result->store_string(JSON::stringify(expected, "\t", true, true) + "\n");
	CHECK_EQ(result->get_error(), OK);
}
