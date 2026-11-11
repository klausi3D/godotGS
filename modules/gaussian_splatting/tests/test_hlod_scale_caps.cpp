extern "C" int test_hlod_scale_caps_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/gs_vector_alloc.h"
#include "../lod/hlod_runtime_selection.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/hlod_node_pack.h"
#include <cstring>
#include <vector>

// Option D step D1 (docs/architecture/adr-one-route-many-producers.md 12.6.5): the HLOD route carries trees beyond its
// old hard limits of 32,768 nodes and 1,048,576 leaf splats, and a quantized node keeps its id above 65,535 (#1413).
// CPU only: the packer, the selector and the admission of a capture. The GPU read of the widened id is the shader
// mirror below (the record bytes the shader reads), plus the layout guard (tests/ci/check_gaussian_layout_sync.py).
namespace TestHlodScaleCaps {
// Neither REQUIRE nor CHECK aborts in this build (DOCTEST_CONFIG_NO_EXCEPTIONS).
#define GS_NEED(cond)                                  \
	do {                                               \
		if (!(cond)) {                                 \
			FAIL("requirement failed: ", #cond);       \
			return;                                    \
		}                                              \
	} while (0)

// The record's chunk index exactly as shaders/includes/quantization_dequant.glsl reads it: GaussianQuantized is
// read as 32-bit words; position_chunk.y is the word at byte 4 (index bits 0..15 in its high half) and chunk_id_hi
// is the word at byte 28 (index bits 16..31 in its low half). Byte offsets, not field names, so this mirror reads
// what the GPU reads whatever the host fields are called.
static uint32_t shader_chunk_index(const PackedGaussianQuantized &p_record) {
	uint32_t word4 = 0, word28 = 0;
	memcpy(&word4, reinterpret_cast<const uint8_t *>(&p_record) + 4, sizeof(uint32_t));
	memcpy(&word28, reinterpret_cast<const uint8_t *>(&p_record) + 28, sizeof(uint32_t));
	return (word4 >> 16u) | ((word28 & 0xFFFFu) << 16u);
}

static uint32_t word_at(const PackedGaussianQuantized &p_record, size_t p_offset) {
	uint32_t word = 0;
	memcpy(&word, reinterpret_cast<const uint8_t *>(&p_record) + p_offset, sizeof(uint32_t));
	return word;
}

TEST_CASE("[GaussianSplatting][HLOD][D1][WorldIO] a quantized node pack keeps node ids at and above 65,536 (#1413)") {
	// Node ids around the old 16-bit boundary and well past it. Only these nodes carry a payload.
	const uint32_t ids[] = { 0u, 1u, 65534u, 65535u, 65536u, 65537u, 70000u, 131071u, 131072u };
	const uint32_t node_count = 131073u;
	const uint32_t splats = 4u;
	GaussianSplatHlodRuntimeData data;
	GS_NEED(gs_resize_or_fail(data.nodes, node_count, "D1 pack nodes"));
	data.leaf_count = splats;
	Gaussian gaussians[splats];
	for (uint32_t i = 0; i < splats; ++i) {
		gaussians[i].position = Vector3(float(i) * 0.5f, 1.0f - float(i) * 0.25f, 2.0f);
		gaussians[i].scale = Vector3(0.125f, 0.0625f, 0.25f);
		gaussians[i].opacity = 0.75f;
		gaussians[i].sh_dc = Color(0.25f, 0.5f, 0.75f, 1.0f);
	}
	gs_hlod_pack::Params params;
	params.quantized = true;
	params.position_bits = 16;
	params.scale_bits = 12;
	gs_hlod_pack::PayloadView view;
	view.gaussians = gaussians;
	for (uint32_t id : ids) {
		CAPTURE(id);
		GaussianSplatHlodRuntimeData::Node &node = data.nodes.write[id];
		node.topology.payload_first = 0;
		node.topology.payload_count = splats;
		PackedGaussianQuantized records[splats];
		memset(records, 0xA5, sizeof(records)); // The packer must write every byte the shader reads.
		ChunkMetaGPU meta;
		ChunkQuantizationGPU quant;
		SHCompressionMetrics metrics;
		gs_hlod_pack::Output out;
		out.quantized = records;
		out.meta = &meta;
		out.quant = &quant;
		out.metrics = &metrics;
		GS_NEED(gs_hlod_pack::pack_node(data, id, params, 4096u, out, &view));
		// The node table and the quantization table are indexed by the node id (ChunkMetaGPU.quant_base).
		CHECK(meta.quant_base == id);
		CHECK(meta.splat_count == splats);
		for (uint32_t i = 0; i < splats; ++i) {
			CAPTURE(i);
			// What the shader reads back is the node id, not the id modulo 65,536.
			CHECK(shader_chunk_index(records[i]) == id);
			// Below 65,536 the record is the layout it always was: the high half-word stays zero (the former padding),
			// so every record packed before the change decodes to the same index.
			if (id < 65536u) {
				CHECK((word_at(records[i], 28) & 0xFFFFu) == 0u);
			}
			// The other half of the word at byte 28 stays deterministic padding.
			CHECK((word_at(records[i], 28) >> 16u) == 0u);
		}
		node.topology.payload_count = 0;
	}
}

// A synthetic tree beyond the old caps: 102,985 nodes (old cap 32,768, and above 65,535) and 1,114,112 leaf splats
// (old cap 1,048,576). Levels of 1, 8, 64, 512, 4,096, 32,768 interior nodes (8 children each, 2 on the last interior
// level) and 65,536 leaves, in breadth-first order so each node's children are contiguous. Leaf j sits at x = j.
struct BigTree {
	GaussianSplatHlodRuntimeData data;
	std::vector<uint32_t> first_leaf; // Per node: the first leaf (0..65,535) it covers.
	std::vector<uint32_t> leaf_span; // Per node: how many leaves it covers.
	uint32_t leaf_base = 0; // Node id of leaf 0.
	static constexpr uint32_t kLeaves = 65536u;
	static constexpr uint32_t kLeafSplats = 17u;
	static constexpr uint32_t kInteriorSplats = 8u;
};

static bool build_big_tree(BigTree &r_tree) {
	const uint32_t level_size[] = { 1u, 8u, 64u, 512u, 4096u, 32768u, 65536u };
	const uint32_t levels = 7;
	uint32_t offset[7] = {};
	uint32_t total = 0;
	for (uint32_t l = 0; l < levels; ++l) {
		offset[l] = total;
		total += level_size[l];
	}
	if (!gs_resize_or_fail(r_tree.data.nodes, total, "D1 big tree")) {
		return false;
	}
	r_tree.first_leaf.assign(total, 0u);
	r_tree.leaf_span.assign(total, 0u);
	r_tree.leaf_base = offset[levels - 1];
	uint64_t interior_payload = 0;
	for (uint32_t l = 0; l < levels; ++l) {
		const uint32_t span = BigTree::kLeaves / level_size[l];
		for (uint32_t j = 0; j < level_size[l]; ++j) {
			const uint32_t id = offset[l] + j;
			GaussianSplatHlodRuntimeData::Node &n = r_tree.data.nodes.write[id];
			const uint32_t a = j * span;
			r_tree.first_leaf[id] = a;
			r_tree.leaf_span[id] = span;
			n.bounds_center = Vector3(float(a) + float(span - 1u) * 0.5f, 0.0f, 0.0f);
			n.topology.radius = float(span) * 0.5f + 0.5f;
			n.topology.parent = l == 0 ? gs_hlod::kNoParent : offset[l - 1] + j / (level_size[l] / level_size[l - 1]);
			n.footprint = 1.0;
			if (l + 1 < levels) {
				const uint32_t fanout = level_size[l + 1] / level_size[l];
				n.topology.first_child = offset[l + 1] + j * fanout;
				n.topology.child_count = fanout;
				n.topology.geometric_error = n.topology.radius * 0.01f;
				n.topology.payload_count = BigTree::kInteriorSplats;
				n.topology.payload_first = uint64_t(BigTree::kLeaves) * BigTree::kLeafSplats + interior_payload;
				interior_payload += BigTree::kInteriorSplats;
			} else {
				n.topology.payload_count = BigTree::kLeafSplats;
				n.topology.payload_first = uint64_t(j) * BigTree::kLeafSplats;
			}
		}
	}
	r_tree.data.leaf_count = BigTree::kLeaves * BigTree::kLeafSplats;
	r_tree.data.parent_count = uint32_t(interior_payload);
	return true;
}

TEST_CASE("[GaussianSplatting][HLOD][D1][WorldIO] the runtime cut selects a complete cut over a tree beyond the old node and leaf-splat caps") {
	BigTree tree;
	GS_NEED(build_big_tree(tree));
	const uint32_t count = uint32_t(tree.data.nodes.size());
	CHECK(count == 102985u);
	CHECK(count > 65536u); // Above the old 32,768 cap and above the 16-bit quantized id range.
	CHECK(tree.data.leaf_count == 1114112u); // Above the old 1,048,576 leaf-splat cap.
	gs_hlod_runtime::CutHistory history;
	// The selector's workspace is admitted for this tree (it refused anything above 32,768 nodes).
	GS_NEED(history.reserve(count));
	gs_hlod_runtime::CutRequest request;
	request.frustum_enabled = false;
	request.camera = Vector3(float(BigTree::kLeaves - 1u), 1.0f, 0.0f); // Next to the last leaf.
	request.focal_pixels = 1000.0;
	request.tau_pixels = 1.0;
	request.record_budget = 1.0e18;
	request.splat_budget = UINT64_MAX / 4u;
	request.node_budget = count;
	const gs_hlod_runtime::CutHistory::Result cut = history.select(tree.data, request);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	GS_NEED(cut.selected_count > 0);
	// Property, not mechanism: the cut covers every leaf exactly once, its splat count is the sum of its nodes, and it
	// reaches the leaves beyond node id 65,535 next to the camera while it stays coarse far away.
	std::vector<uint8_t> covered(BigTree::kLeaves, 0u);
	uint64_t splats = 0;
	uint32_t max_selected = 0;
	bool any_interior = false;
	for (uint32_t i = 0; i < cut.selected_count; ++i) {
		const uint32_t node = history.selected_node(i);
		GS_NEED(node < count);
		max_selected = MAX(max_selected, node);
		any_interior = any_interior || node < tree.leaf_base;
		splats += tree.data.nodes[node].topology.payload_count;
		for (uint32_t leaf = tree.first_leaf[node]; leaf < tree.first_leaf[node] + tree.leaf_span[node]; ++leaf) {
			if (covered[leaf]++ != 0u) {
				FAIL("leaf ", leaf, " is covered twice (node ", node, ")");
				return;
			}
		}
	}
	for (uint32_t leaf = 0; leaf < BigTree::kLeaves; ++leaf) {
		if (covered[leaf] != 1u) {
			FAIL("leaf ", leaf, " is not covered by the cut");
			return;
		}
	}
	CHECK(splats == cut.selected_splats);
	CHECK(max_selected == count - 1u); // The leaf next to the camera, node id 102,984.
	CHECK(any_interior);
}
} // namespace TestHlodScaleCaps
