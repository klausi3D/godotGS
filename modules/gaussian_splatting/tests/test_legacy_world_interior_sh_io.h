#pragma once

// Golden values come from the actual historical producer's native BakeResult,
// not a new bake or a hand-written positive header/tree. See fixture provenance.
#include "test_gaussian_layout_fingerprint.h"
#include "core/io/json.h"

namespace TestAuthenticLegacyInteriorSh {
static bool numeric_array(const Variant &p_value, int p_size, Array &r_array) {
	if (p_value.get_type() != Variant::ARRAY) {
		FAIL("captured numeric array is required");
		return false;
	}
	r_array = p_value;
	if (r_array.size() != p_size) {
		FAIL("captured numeric array cardinality changed");
		return false;
	}
	for (int i = 0; i < p_size; i++) {
		if (r_array[i].get_type() != Variant::INT && r_array[i].get_type() != Variant::FLOAT) {
			FAIL("captured array must contain numeric values");
			return false;
		}
	}
	return true;
}

static bool check_vector(const Vector3 &p_got, const Variant &p_expected) {
	Array values;
	if (!numeric_array(p_expected, 3, values)) {
		return false;
	}
	CHECK(p_got.is_equal_approx(Vector3(real_t(values[0]), real_t(values[1]), real_t(values[2]))));
	return true;
}

static bool check_gaussian(const Gaussian &p_got, const Dictionary &p_expected) {
	for (const char *key : {"position", "opacity", "scale", "area", "rotation", "sh_dc", "sh_first", "normal",
			 "stroke_age", "padding", "brush_axes", "painterly_meta", "render_meta", "padding2"}) {
		if (!p_expected.has(key)) {
			FAIL("captured Gaussian field is missing: ", key);
			return false;
		}
	}
	if (!check_vector(p_got.position, p_expected["position"]) ||
			!check_vector(p_got.scale, p_expected["scale"]) || !check_vector(p_got.normal, p_expected["normal"])) {
		return false;
	}
	CHECK_EQ(p_got.opacity, float(p_expected["opacity"]));
	CHECK_EQ(p_got.area, float(p_expected["area"]));
	Array rotation, dc, axes, padding;
	if (!numeric_array(p_expected["rotation"], 4, rotation) || !numeric_array(p_expected["sh_dc"], 4, dc) ||
			!numeric_array(p_expected["brush_axes"], 2, axes) || !numeric_array(p_expected["padding2"], 2, padding)) {
		return false;
	}
	CHECK(p_got.rotation.is_equal_approx(Quaternion(real_t(rotation[0]), real_t(rotation[1]), real_t(rotation[2]), real_t(rotation[3]))));
	CHECK(p_got.sh_dc.is_equal_approx(Color(float(dc[0]), float(dc[1]), float(dc[2]), float(dc[3]))));
	CHECK(p_got.brush_axes.is_equal_approx(Vector2(real_t(axes[0]), real_t(axes[1]))));
	CHECK_EQ(p_got._padding2[0], float(padding[0]));
	CHECK_EQ(p_got._padding2[1], float(padding[1]));
	if (p_expected["sh_first"].get_type() != Variant::ARRAY) {
		FAIL("captured first-order SH arrays are required");
		return false;
	}
	const Array first = p_expected["sh_first"];
	if (first.size() != 3) {
		FAIL("three captured first-order vectors are required");
		return false;
	}
	for (int i = 0; i < 3; i++) {
		if (!check_vector(p_got.sh_1[i], first[i])) {
			return false;
		}
	}
	CHECK_EQ(p_got.stroke_age, float(p_expected["stroke_age"]));
	CHECK_EQ(p_got._padding, float(p_expected["padding"]));
	CHECK_EQ(p_got.painterly_meta, uint32_t(p_expected["painterly_meta"]));
	CHECK_EQ(p_got.render_meta, uint32_t(p_expected["render_meta"]));
	return true;
}

static bool check_payload(const LocalVector<Gaussian> &p_gaussians, const LocalVector<Vector3> &p_sh,
		const Array &p_expected_gaussians, const Array &p_expected_sh, bool p_reverse = false) {
	if (p_gaussians.size() != uint32_t(p_expected_gaussians.size()) || p_sh.size() != uint32_t(p_expected_sh.size()) ||
			p_sh.size() != p_gaussians.size() * 5u) {
		FAIL("captured Gaussian and higher-order cardinalities must match");
		return false;
	}
	for (uint32_t i = 0; i < p_gaussians.size(); i++) {
		const uint32_t expected_index = p_reverse ? p_gaussians.size() - 1u - i : i;
		if (p_expected_gaussians[expected_index].get_type() != Variant::DICTIONARY ||
				!check_gaussian(p_gaussians[i], p_expected_gaussians[expected_index])) {
			FAIL("captured Gaussian object could not be checked");
			return false;
		}
		for (uint32_t k = 0; k < 5u; k++) {
			if (!check_vector(p_sh[i * 5u + k], p_expected_sh[expected_index * 5u + k])) {
				return false;
			}
		}
	}
	return true;
}

static bool check_node(const GaussianSplatHlodTree &p_tree, const GaussianSplatHlodNode &p_node, const Dictionary &p_expected) {
	for (const char *key : {"e", "ix", "iy", "iz", "aabb_min", "aabb_max", "radius", "geometric_error", "error_chosen",
			 "error_rejected", "parent", "first_child", "child_count", "height", "payload_first", "payload_count", "flags",
			 "overlap_footprint", "cell_center"}) {
		if (!p_expected.has(key)) {
			FAIL("captured node field is missing: ", key);
			return false;
		}
	}
	CHECK_EQ(p_node.cell.e, int32_t(p_expected["e"]));
	CHECK_EQ(p_node.cell.ix, int64_t(p_expected["ix"]));
	CHECK_EQ(p_node.cell.iy, int64_t(p_expected["iy"]));
	CHECK_EQ(p_node.cell.iz, int64_t(p_expected["iz"]));
	CHECK_EQ(p_node.parent, uint32_t(p_expected["parent"]));
	CHECK_EQ(p_node.first_child, uint32_t(p_expected["first_child"]));
	CHECK_EQ(p_node.child_count, uint32_t(p_expected["child_count"]));
	CHECK_EQ(p_node.height, uint32_t(p_expected["height"]));
	CHECK_EQ(p_node.payload_first, uint64_t(p_expected["payload_first"]));
	CHECK_EQ(p_node.payload_count, uint32_t(p_expected["payload_count"]));
	CHECK_EQ(p_node.flags, uint32_t(p_expected["flags"]));
	CHECK_EQ(p_node.radius, float(p_expected["radius"]));
	CHECK_EQ(p_node.geometric_error, float(p_expected["geometric_error"]));
	CHECK_EQ(p_node.error_chosen, float(p_expected["error_chosen"]));
	CHECK_EQ(p_node.error_rejected, float(p_expected["error_rejected"]));
	CHECK_EQ(p_node.overlap_footprint, float(p_expected["overlap_footprint"]));
	if (!check_vector(p_node.aabb_min, p_expected["aabb_min"]) || !check_vector(p_node.aabb_max, p_expected["aabb_max"])) {
		return false;
	}
	Array center;
	if (!numeric_array(p_expected["cell_center"], 3, center)) {
		return false;
	}
	double actual_center[3];
	p_tree.node_cell_center(p_node, actual_center);
	for (int i = 0; i < 3; i++) {
		CHECK_EQ(actual_center[i], double(center[i]));
	}
	return true;
}

static bool check_world(const Ref<GaussianSplatWorld> &p_world, const Dictionary &p_expected, bool p_staged) {
	if (p_world.is_null() || !p_world->has_hlod_tree() || p_world->get_splat_count() != 2) {
		FAIL("actual production loader must publish the authentic branching HLOD world");
		return false;
	}
	const GaussianSplatHlodTree &tree = p_world->get_hlod_tree();
	CHECK_EQ(tree.leaf_splat_count, 2u);
	CHECK_EQ(tree.interior_splat_count, 1u);
	CHECK_EQ(tree.sh_high_order_count, 5u);
	CHECK_EQ(tree.bake_rule_version, uint32_t(p_expected["bake_rule_version"]));
	CHECK_EQ(tree.top_level_nodes.size(), 0u);
	CHECK_EQ(tree.instances.size(), 0u);
	CHECK(p_world->get_metadata() == Dictionary(p_expected["metadata"]));
	Array origin;
	if (!numeric_array(p_expected["origin"], 3, origin)) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		CHECK_EQ(tree.origin[i], double(origin[i]));
	}
	String reason;
	CHECK(gs_hlod_validate_tree(tree, &reason));
	const Array nodes = p_expected["nodes"];
	if (tree.nodes.size() != 3u || nodes.size() != 3) {
		FAIL("three genuine captured node records are required");
		return false;
	}
	for (uint32_t i = 0; i < tree.nodes.size(); i++) {
		if (nodes[i].get_type() != Variant::DICTIONARY || !check_node(tree, tree.nodes[i], nodes[i])) {
			FAIL("actual node record could not be checked against the producer");
			return false;
		}
	}
	LocalVector<Gaussian> interiors;
	LocalVector<Vector3> interior_sh;
	if (tree.read_interior_payload(interiors, interior_sh) != OK || interiors.size() != 1u || interior_sh.size() != 5u ||
			!check_payload(interiors, interior_sh, p_expected["interior_gaussians"], p_expected["interior_sh_high_order"])) {
		FAIL("actual resident or file-backed interior and SH must match the producer");
		return false;
	}
	LocalVector<Gaussian> leaves;
	LocalVector<Vector3> leaf_sh;
	uint32_t first = 0u, high = 0u;
	CHECK_EQ(p_world->has_chunk_payload_source(), p_staged);
	if (!p_staged) {
		const Ref<GaussianData> data = p_world->get_gaussian_data();
		if (data.is_null() || !data->capture_chunk_snapshot(0u, 2u, leaves, leaf_sh, first, high)) {
			FAIL("actual resident capture must decode the authentic leaves and SH");
			return false;
		}
		CHECK_EQ(data->get_sh_degree(), 2u);
	} else {
		CHECK(p_world->get_gaussian_data().is_null());
		const Ref<StagedFileChunkPayloadSource> source = p_world->get_chunk_payload_source();
		if (source.is_null() || !source->is_valid()) {
			FAIL("actual staged source must remain valid");
			return false;
		}
		for (int repeat = 0; repeat < 2; repeat++) {
			if (!source->capture_chunk_snapshot(0u, 2u, leaves, leaf_sh, first, high) ||
						!check_payload(leaves, leaf_sh, p_expected["leaf_gaussians"], p_expected["leaf_sh_high_order"])) {
				FAIL("actual cached contiguous capture must restore leaf frames and SH");
				return false;
			}
			CHECK_EQ(first, 3u);
			CHECK_EQ(high, 5u);
			CHECK_EQ(source->get_file_open_count(), 1u);
		}
		const uint32_t reverse[] = {1u, 0u};
		if (!source->capture_indexed_chunk_snapshot(reverse, 2u, leaves, leaf_sh, first, high) ||
				!check_payload(leaves, leaf_sh, p_expected["leaf_gaussians"], p_expected["leaf_sh_high_order"], true)) {
			FAIL("actual indexed capture must preserve reversed leaf identities, frames and SH");
			return false;
		}
		CHECK_EQ(first, 3u);
		CHECK_EQ(high, 5u);
		CHECK_EQ(source->get_file_open_count(), 1u);
		return true;
	}
	CHECK_EQ(first, 3u);
	CHECK_EQ(high, 5u);
	return check_payload(leaves, leaf_sh, p_expected["leaf_gaussians"], p_expected["leaf_sh_high_order"]);
}
} // namespace TestAuthenticLegacyInteriorSh

TEST_CASE("[GaussianSplatting][WorldIO] captured legacy HLOD interiors SH frames and node tables load") {
	using namespace TestAuthenticLegacyInteriorSh;
	using namespace TestGaussianLayoutFingerprint;
	const String directory = "tests/fixtures/legacy_world_producer/interior_sh";
	const String expected_path = directory.path_join("bake-result.json");
	if (FileAccess::get_sha256(expected_path) != "4f3b08898ae6d1da5d736c0d61d987fcec2da7c0d7ab52925de7f96c07182b7b") {
		FAIL("authenticated actual old BakeResult fixture is missing or changed");
		return;
	}
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(FileAccess::get_file_as_string(expected_path)) != OK || json->get_data().get_type() != Variant::DICTIONARY) {
		FAIL("actual old BakeResult must parse as its captured object");
		return;
	}
	const Dictionary expected = json->get_data();
	for (const char *key : {"nodes", "leaf_gaussians", "leaf_sh_high_order", "interior_gaussians", "interior_sh_high_order"}) {
		if (!expected.has(key) || expected[key].get_type() != Variant::ARRAY) {
			FAIL("captured expected payload arrays must be present");
			return;
		}
	}
	if (!expected.has("metadata") || expected["metadata"].get_type() != Variant::DICTIONARY ||
			!expected.has("origin") || !expected.has("bake_rule_version")) {
		FAIL("captured expected metadata/frame contract must be present");
		return;
	}
	const char *hashes[] = {
		"f62603b91abf7837bca60c49150f0bc062640ce091523aadce9e0f770b8f3958",
		"06a590eaa5584bae36a4218d2da3c199be8f62bba26f4dc983d61120b098ac14",
		"e97779b1c30a95abe279c21e57243748862b5ac419e766adfb3b3d01b2188585",
	};
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (int mode = 0; mode < 3; mode++) {
		const String suffix = mode == 0 ? "streamable" : mode == 1 ? "resident" : "compressed";
		const String path = directory.path_join("hlod_v2_interior_sh_" + suffix + ".gsplatworld");
		if (FileAccess::get_sha256(path) != hashes[mode]) {
			FAIL("authenticated old producer world is missing or changed");
			return;
		}
		CHECK_EQ(read_u32_at(path, 4u), 2u);
		CHECK_EQ(read_u32_at(path, 16u), 2u);
		CHECK_EQ(read_u32_at(path, 20u), 3u);
		CHECK_EQ(read_u32_at(path, 24u), 5u);
		CHECK_EQ(read_u64_at(path, 56u), 184u);
		CHECK_EQ(read_u32_at(path, 132u), 1u);
		CHECK_EQ(read_u32_at(path, 144u), 3u);
		CHECK_EQ(read_u64_at(path, 136u), mode == 2 ? 528u : 796u);
		CHECK_EQ(read_u32_at(path, 148u), 0u);
		CHECK_EQ(read_u32_at(path, 168u), 0u);
		Error err = ERR_BUG;
		Ref<GaussianSplatWorld> normal = loader.load(path, "", &err);
		CHECK_EQ(err, OK);
		// Each checker records FAIL before returning on a missing/invalid object.
		// Keep the independent resident route and other modes running for REDs.
		check_world(normal, expected, mode == 0);
		err = ERR_BUG;
		Ref<GaussianSplatWorld> resident = loader.load_resident(path, &err);
		CHECK_EQ(err, OK);
		check_world(resident, expected, false);
	}
}
