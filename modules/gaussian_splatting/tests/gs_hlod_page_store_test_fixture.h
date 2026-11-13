#pragma once

#include "test_macros.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gs_vector_alloc.h"
#include "../io/gs_world_writer_v4.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"

#include <utility>

// Synthetic topology and source attributes, serialized by the production V4
// writer. The oracle regenerates attributes and calls the codec directly; it
// never asks either V4 reader path for its expected bytes.
namespace TestHlodPageStore {

struct Fixture {
	String directory;
	String scratch;
	String path;
	Ref<GaussianSplatWorld> world;
	Ref<GaussianSplatHlodRuntimeData> snapshot;
	Ref<GaussianSplatHlodPagedSource> source;
	LocalVector<gs_world_writer::V4Node> nodes;
	uint32_t node_count = 0;
	uint32_t splats_per_node = 0;
	uint32_t high_order = 0;
	uint32_t degree = 0;
	bool gsq = false;

	static void remove_files(const String &p_directory) {
		Ref<DirAccess> dir = DirAccess::open(p_directory);
		if (dir.is_null() || dir->list_dir_begin() != OK) {
			return;
		}
		for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
			if (name != "." && name != "..") {
				DirAccess::remove_absolute(p_directory.path_join(name));
			}
		}
		dir->list_dir_end();
	}

	~Fixture() {
		// Callers declare the fixture before its streamer/store, so their workers
		// and file handles are drained before this cleanup.
		snapshot.unref();
		world.unref();
		source.unref();
		if (!directory.is_empty()) {
			remove_files(scratch);
			DirAccess::remove_absolute(scratch);
			remove_files(directory);
			DirAccess::remove_absolute(directory);
		}
	}

	void generate_relative(uint32_t p_node, LocalVector<Gaussian> &r_gaussians, LocalVector<Vector3> &r_sh) const {
		r_gaussians.resize(splats_per_node);
		r_sh.resize(splats_per_node * high_order);
		for (uint32_t i = 0; i < r_gaussians.size(); ++i) {
			Gaussian g;
			// Dyadic endpoints preserve leaf centre reconstruction exactly. The
			// non-dyadic scales/SH ensure GSQ is observably lossy.
			g.position = Vector3(i & 1u ? 0.125f : -0.125f, i & 2u ? 0.125f : -0.125f, i & 4u ? 0.125f : -0.125f);
			g.scale = Vector3(0.03125f, 0.043f + float(i % 3u) * 0.003f, 0.071f);
			g.opacity = 0.6f;
			g.sh_dc = Color(0.25f + float(p_node % 8u) * 0.03125f, 0.5f, 0.75f, 1.0f);
			if (degree != 0u) {
				g.sh_1[0] = Vector3(0.13f, -0.07f, 0.01f);
			}
			g.render_meta = p_node & 1u ? GAUSSIAN_DC_ENCODING_LINEAR_RGB : GAUSSIAN_DC_ENCODING_LEGACY_BIAS;
			r_gaussians[i] = g;
			for (uint32_t k = 0; k < high_order; ++k) {
				r_sh[i * high_order + k] = Vector3(float(k + 1u) * 0.013f, -0.027f, float(i % 3u) * 0.019f);
			}
		}
	}

	bool expected_payload(uint32_t p_node, Vector<Gaussian> &r_gaussians, Vector<Vector3> &r_sh) const {
		if (p_node >= nodes.size() || nodes.size() != node_count) {
			FAIL("D3-2 identity oracle requires an existing fixture node");
			return false;
		}
		LocalVector<Gaussian> gaussians;
		LocalVector<Vector3> sh;
		generate_relative(p_node, gaussians, sh);
		if (gsq) {
			LocalVector<uint8_t> encoded;
			String reason;
			gs_gsq::DecodeContext decoder;
			if (gs_gsq::encode_page(gaussians.ptr(), sh.ptr(), splats_per_node, degree,
						gs_gsq::EncodeParams(), encoded, reason) != OK ||
					decoder.decode_range(encoded.ptr(), encoded.size(), 0, splats_per_node,
							gaussians.ptr(), sh.ptr(), reason, true) != OK) {
				FAIL("D3-2 independent GSQ oracle encode/decode failed: ", reason.utf8().get_data());
				return false;
			}
		}
		const GaussianSplatHlodNode &node = nodes[p_node].topology;
		if (node.is_leaf()) {
			GaussianSplatHlodTree lattice;
			double center[3];
			lattice.node_cell_center(node, center);
			// Independent statement of the base V4 leaf convention. Do not call
			// reconstruct_leaf_positions: that is part of the code under test.
			for (Gaussian &g : gaussians) {
				for (int axis = 0; axis < 3; ++axis) {
					g.position[axis] = real_t(double(g.position[axis]) + center[axis]);
				}
			}
		}
		if (!gs_resize_or_fail(r_gaussians, splats_per_node, "D3-2 identity oracle gaussians") ||
				!gs_resize_or_fail(r_sh, uint64_t(splats_per_node) * high_order, "D3-2 identity oracle SH")) {
			FAIL("D3-2 identity oracle allocation failed");
			return false;
		}
		for (uint32_t i = 0; i < gaussians.size(); ++i) {
			r_gaussians.write[i] = gaussians[i];
		}
		for (uint32_t i = 0; i < sh.size(); ++i) {
			r_sh.write[i] = sh[i];
		}
		return true;
	}

	// depth 2 = 7 nodes; depth 5 = 63 nodes. Count and codec are per fixture,
	// with no changes to runtime defaults. Payload generation holds one node.
	bool build(bool p_gsq = false, uint32_t p_depth = 2, uint32_t p_splats = 256, uint32_t p_degree = 3) {
		if (!directory.is_empty() || p_depth > 8u || p_splats == 0u || p_splats > gs_hlod::kMaxNodeSplats ||
				(p_degree != 0u && p_degree != 3u) || !OS::get_singleton()) {
			FAIL("D3-2 fixture requires a fresh instance, supported dimensions and an OS clock");
			return false;
		}
		gsq = p_gsq;
		degree = p_degree;
		high_order = gs_gsq::sh_high_order_for_degree(degree);
		splats_per_node = p_splats;
		node_count = (1u << (p_depth + 1u)) - 1u;
		const uint32_t leaf_nodes = 1u << p_depth;
		const uint32_t interior_nodes = leaf_nodes - 1u;
		static uint32_t serial = 0;
		directory = OS::get_singleton()->get_temp_path().path_join("gs_d3_2_" +
				uitos(OS::get_singleton()->get_ticks_usec()) + "_" + uitos(serial++));
		scratch = directory.path_join("scratch");
		path = directory.path_join("world.gsplatworld");
		if (DirAccess::make_dir_recursive_absolute(scratch) != OK) {
			FAIL("D3-2 fixture could not create its isolated scratch directory");
			return false;
		}
		GaussianSplatHlodTree tree;
		tree.nodes.resize(node_count);
		tree.leaf_splat_count = leaf_nodes * splats_per_node;
		tree.interior_splat_count = interior_nodes * splats_per_node;
		tree.sh_high_order_count = high_order;
		nodes.resize(node_count);
		for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
			uint32_t depth = 0;
			for (uint32_t level = i + 1u; level > 1u; level >>= 1u) {
				++depth;
			}
			GaussianSplatHlodNode &node = tree.nodes[i];
			const bool leaf = depth == p_depth;
			node.cell.e = int32_t(p_depth + 2u - depth);
			if (i != 0u) {
				const GaussianSplatHlodNode &parent = tree.nodes[(i - 1u) / 2u];
				node.cell.ix = parent.cell.ix * 2 + int64_t((i - 1u) % 2u);
			}
			const float half = float(1u << uint32_t(node.cell.e - 1));
			node.aabb_min = Vector3(-half, -half, -half);
			node.aabb_max = Vector3(half, half, half);
			node.radius = 4.0f * half;
			node.parent = i == 0u ? gs_hlod::kNoParent : (i - 1u) / 2u;
			node.first_child = leaf ? 0u : 2u * i + 1u;
			node.child_count = leaf ? 0u : 2u;
			node.height = p_depth - depth;
			node.flags = leaf ? gs_hlod::NODE_KIND_LEAF : gs_hlod::NODE_KIND_MERGED;
			node.geometric_error = leaf ? 0.0f : float(1u << node.height);
			node.payload_count = splats_per_node;
			node.payload_first = uint64_t(leaf ? i - interior_nodes : leaf_nodes + i) * splats_per_node;
			nodes[i].topology = node;
		}
		String reason;
		if (!gs_hlod_validate_tree(tree, &reason)) {
			FAIL("D3-2 synthetic tree validation failed: ", reason.utf8().get_data());
			return false;
		}
		gs_world_writer::PagedV4Spec spec;
		spec.node_count = node_count;
		spec.leaf_splat_count = tree.leaf_splat_count;
		spec.interior_splat_count = tree.interior_splat_count;
		spec.sh_degree = degree;
		spec.sh_first_order = degree == 0u ? 0u : 3u;
		spec.sh_high_order = high_order;
		const float edge = float(1u << (p_depth + 2u));
		spec.bounds = AABB(Vector3(), Vector3(edge, edge, edge));
		gs_world_writer::WriterV4 writer;
		if (writer.begin_paged(path, scratch, spec, gsq ? gs_world_writer::gsq_v1_codec_params() :
					gs_world_writer::CodecParams(), gs_world_writer::Limits::unbounded()) != OK) {
			FAIL("D3-2 production V4 writer begin failed: ", writer.get_report().reason.utf8().get_data());
			return false;
		}
		for (uint32_t i = 0; i < nodes.size(); ++i) {
			LocalVector<Gaussian> gaussians;
			LocalVector<Vector3> sh;
			generate_relative(i, gaussians, sh);
			gs_world_writer::V4BodyDesc desc;
			desc.is_leaf = nodes[i].topology.is_leaf();
			desc.segment.depth = p_depth - nodes[i].topology.height;
			tree.node_cell_center(nodes[i].topology, desc.cell_center);
			if (desc.is_leaf) {
				for (Gaussian &g : gaussians) {
					for (int axis = 0; axis < 3; ++axis) {
						g.position[axis] = real_t(double(g.position[axis]) + desc.cell_center[axis]);
					}
				}
			}
			if (writer.append_node(desc, gaussians.ptr(), sh.ptr(), splats_per_node, nodes[i].body) != OK) {
				FAIL("D3-2 production V4 writer append failed: ", writer.get_report().reason.utf8().get_data());
				return false;
			}
		}
		if (writer.finalize(nodes) != OK) {
			FAIL("D3-2 production V4 writer finalize failed: ", writer.get_report().reason.utf8().get_data());
			return false;
		}
		source.instantiate();
		if (source.is_null()) {
			FAIL("D3-2 fixture could not allocate its paged source");
			return false;
		}
		Dictionary metadata;
		if (source->open(path, tree, metadata, reason) != OK) {
			FAIL("D3-2 production V4 package must open: ", reason.utf8().get_data());
			return false;
		}
		world.instantiate();
		if (world.is_null()) {
			FAIL("D3-2 fixture could not allocate its paged world");
			return false;
		}
		if (world->set_hlod_paged_package(std::move(tree), source) != OK ||
				world->capture_hlod_runtime_snapshot(snapshot, reason) != OK || snapshot.is_null()) {
			FAIL("D3-2 paged V4 runtime snapshot must capture: ", reason.utf8().get_data());
			return false;
		}
		return true;
	}
};

} // namespace TestHlodPageStore
