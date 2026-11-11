#pragma once

// D2-1 tests first: no tiler, codec, writer or lane-cache changes.
// Intended API (not implemented in phase 1): BakeParams gains
//   const GaussianSplatHlodCell *forced_root = nullptr;
// The borrowed cell lives through the synchronous bake. nullptr keeps automatic root
// selection; non-null fixes an ordinary lattice root, with half-open centre containment.
// It must not shrink to the input bounds or introduce an origin-centred root flag.
// MERGE_ONLY jobs must also reject bake entry after a failed RAM reservation;
// callers of the core API cannot rely on GaussianSplatWorld's admission wrapper.
// These references were checked at 0488f90a27bfc44e0b7ffab86f1d46491d5e91df:
// bake.cpp:248-252 offline refusal, :283-288 root choice, :427-429/:636-638
// input-index ties, :720-782 child-local merge, :833-841 rule-1 payload/error;
// merge.h:167-173 frozen parameters; job.h:84-125 reservation; tree.h:57-59 rule 1.
// Registration: modules/SCsub globs tests/*.h; [WorldIO] is a strict host-only lane.
// The scan-like SH3 fixture is generated in-test from a fixed integer hash:
// clustered surfaces, blobs and outliers, with no binary asset or cwd dependency.

#include "test_macros.h"

#include "../core/gaussian_splat_hlod_bake.h"
#include "../core/gaussian_splat_hlod_job.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace TestHlodD2Bake {

using namespace gs_hlod;

// REQUIRE does not abort in the default no-exceptions build.
#define D2_NEED(condition) \
	do { \
		if (!(condition)) { \
			FAIL("D2 precondition failed: ", #condition); \
			return; \
		} \
	} while (0)

struct Input {
	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh;
	uint32_t high_count = 0;
	uint32_t first_count = 0;

	BakeInput view() const {
		return { gaussians.ptr(), sh.ptr(), gaussians.size(), high_count, first_count };
	}
};

inline Input synthetic(uint32_t p_count, uint32_t p_high = 0) {
	Input input;
	input.high_count = p_high;
	input.first_count = p_high ? 3u : 0u;
	input.gaussians.resize(p_count);
	input.sh.resize(p_count * p_high);
	for (uint32_t i = 0; i < p_count; ++i) {
		// Interleaved global order, equal importance, eight sparse octants. Each
		// occupied cluster fits a much smaller cell than its ancestor's octant:
		// ignoring forced_root therefore loses several unary interior levels.
		const uint32_t octant = i % 8u;
		const uint32_t j = i / 8u;
		Gaussian &g = input.gaussians[i];
		g.position = Vector3(float((octant & 1u) * 128u) + 1.0f + float(j & 63u) / 8.0f,
				float(((octant >> 1u) & 1u) * 128u) + 1.0f + float((j >> 6u) & 63u) / 8.0f,
				float(((octant >> 2u) & 1u) * 128u) + 1.0f + float((j >> 12u) & 31u) / 8.0f);
		g.scale = Vector3(0.03125f, 0.015625f, 0.0078125f);
		g.rotation = Quaternion(0, 0, 0, 1);
		g.opacity = 0.5f;
		g.sh_dc = Color(float(i % 17u) / 32.0f, float(i % 23u) / 32.0f, 0.25f, 1.0f);
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		for (uint32_t c = 0; c < input.first_count; ++c) {
			g.sh_1[c] = Vector3(float((i + c) % 11u) / 128.0f, -0.03125f, 0.0625f);
		}
		for (uint32_t c = 0; c < p_high; ++c) {
			input.sh[i * p_high + c] = Vector3(float((i + c) % 19u) / 256.0f, float(c) / 256.0f, -0.015625f);
		}
	}
	return input;
}

inline bool valid_result(const BakeResult &p_result) {
	String error;
	if (!gs_hlod_validate_tree(p_result.tree, &error)) {
		FAIL("Invalid D2 tree: ", error.utf8().get_data());
		return false;
	}
	const auto &t = p_result.tree;
	if (t.nodes.is_empty() || !t.interior_resident ||
			p_result.leaf_gaussians.size() != t.leaf_splat_count ||
			p_result.leaf_source_index.size() != t.leaf_splat_count ||
			p_result.leaf_sh_high_order.size() != uint64_t(t.leaf_splat_count) * t.sh_high_order_count ||
			t.interior_gaussians.size() != t.interior_splat_count ||
			t.interior_sh_high_order.size() != uint64_t(t.interior_splat_count) * t.sh_high_order_count) {
		FAIL("D2 bake returned incomplete payload storage");
		return false;
	}
	// A standalone result must use the documented BFS interior layout. The
	// subtree comparator below rebases offsets, so check the physical layout here.
	uint64_t cursor = t.leaf_splat_count;
	uint32_t next_child = 1u;
	for (const auto &node : t.nodes) {
		if (!node.is_leaf()) {
			if (node.first_child != next_child) {
				FAIL("D2 node records are not in canonical BFS order");
				return false;
			}
			next_child += node.child_count;
			if (node.payload_first != cursor) {
				FAIL("D2 interior payload is not in BFS order");
				return false;
			}
			cursor += node.payload_count;
		} else if (node.first_child != 0u) {
			FAIL("D2 leaf has a nonzero first_child field");
			return false;
		}
	}
	return true;
}

inline bool bake(const BakeInput &p_input, const BakeParams &p_params, BakeResult &r_result) {
	String error;
	Error code = OK;
	if (!bake_world(p_input, p_params, r_result, &error, &code)) {
		FAIL("D2 bake failed: ", int(code), " ", error.utf8().get_data());
		return false;
	}
	CHECK(code == OK);
	return valid_result(r_result);
}

inline std::vector<uint32_t> subtree_bfs(const GaussianSplatHlodTree &p_tree, uint32_t p_root) {
	// Only used after valid_result and an explicit root-range check.
	std::vector<uint32_t> ids{ p_root };
	for (size_t i = 0; i < ids.size(); ++i) {
		const auto &node = p_tree.nodes[ids[i]];
		for (uint32_t c = 0; c < node.child_count; ++c) {
			ids.push_back(node.first_child + c);
		}
	}
	return ids;
}

template <class T>
inline void bytes(std::vector<uint8_t> &r_bytes, const T &p_value) {
	const auto *begin = reinterpret_cast<const uint8_t *>(&p_value);
	r_bytes.insert(r_bytes.end(), begin, begin + sizeof(T));
}

// Canonical comparison, NOT a new file format: every node-record field, followed
// by its complete Gaussian and SH payload. Encode fields separately to exclude C++
// struct padding. Only parent/child ids and payload offsets are rebased; geometry,
// height, flags, errors, order and payload bytes may not change. Timing is excluded.
inline std::vector<uint8_t> subtree_bytes(const BakeResult &p_result, uint32_t p_root) {
	const auto &t = p_result.tree;
	const auto ids = subtree_bfs(t, p_root);
	std::vector<uint32_t> local(t.nodes.size(), kNoParent);
	uint64_t leaf_begin = UINT64_MAX;
	uint32_t leaves = 0, interiors = 0;
	for (uint32_t i = 0; i < ids.size(); ++i) {
		local[ids[i]] = i;
		const auto &n = t.nodes[ids[i]];
		if (n.is_leaf()) {
			leaf_begin = MIN(leaf_begin, n.payload_first);
			leaves += n.payload_count;
		} else {
			interiors += n.payload_count;
		}
	}
	std::vector<uint8_t> out;
	bytes(out, t.origin);
	bytes(out, t.bake_rule_version);
	bytes(out, t.sh_high_order_count);
	bytes(out, uint32_t(ids.size()));
	bytes(out, leaves);
	bytes(out, interiors);
	uint64_t interior_cursor = leaves;
	for (uint32_t id : ids) {
		const auto &n = t.nodes[id];
		bytes(out, n.cell.e);
		bytes(out, n.cell.ix);
		bytes(out, n.cell.iy);
		bytes(out, n.cell.iz);
		bytes(out, n.aabb_min);
		bytes(out, n.aabb_max);
		bytes(out, n.radius);
		bytes(out, n.geometric_error);
		bytes(out, n.error_chosen);
		bytes(out, n.error_rejected);
		bytes(out, id == p_root ? kNoParent : local[n.parent]);
		bytes(out, n.is_leaf() ? 0u : local[n.first_child]);
		bytes(out, n.child_count);
		bytes(out, n.height);
		bytes(out, n.is_leaf() ? n.payload_first - leaf_begin : interior_cursor);
		bytes(out, n.payload_count);
		bytes(out, n.flags);
		bytes(out, n.overlap_footprint);
		const uint64_t first = n.is_leaf() ? n.payload_first : n.payload_first - t.leaf_splat_count;
		const auto &g = n.is_leaf() ? p_result.leaf_gaussians : t.interior_gaussians;
		const auto &sh = n.is_leaf() ? p_result.leaf_sh_high_order : t.interior_sh_high_order;
		for (uint32_t i = 0; i < n.payload_count; ++i) {
			bytes(out, g[first + i]);
			for (uint32_t c = 0; c < t.sh_high_order_count; ++c) {
				bytes(out, sh[(first + i) * t.sh_high_order_count + c]);
			}
		}
		if (!n.is_leaf()) {
			interior_cursor += n.payload_count;
		}
	}
	return out;
}

inline void check_rule_one(const BakeResult &p_result) {
	CHECK(kBakeRuleVersion == 1u);
	CHECK(p_result.tree.bake_rule_version == 1u);
	uint32_t interiors = 0;
	for (const auto &n : p_result.tree.nodes) {
		CHECK(n.error_chosen == 0.0f);
		CHECK(n.error_rejected == 0.0f);
		CHECK(n.get_kind() == (n.is_leaf() ? NODE_KIND_LEAF : NODE_KIND_MERGED));
		if (!n.is_leaf()) {
			++interiors;
			CHECK(n.geometric_error > 0.0f);
			uint32_t child_splats = 0;
			for (uint32_t c = 0; c < n.child_count; ++c) {
				const auto &child = p_result.tree.nodes[n.first_child + c];
				child_splats += child.payload_count;
				CHECK(n.geometric_error >= child.geometric_error);
			}
			CHECK(n.payload_count <= MIN(kMaxNodeSplats, (child_splats + 3u) / 4u));
		}
	}
	CHECK(interiors > 0u);
}

inline void check_subtree(const BakeInput &p_input, const BakeParams &p_params,
		const BakeResult &p_full, uint32_t p_root, bool p_expect_smaller_auto_root = false) {
	D2_NEED(p_root > 0u && p_root < p_full.tree.nodes.size());
	const auto &root = p_full.tree.nodes[p_root];
	D2_NEED(!root.is_leaf());
	D2_NEED((root.flags & (kNodeFlagSplitByIndex | kNodeFlagOriginCentredRoot)) == 0u);
	const auto ids = subtree_bfs(p_full.tree, p_root);
	std::vector<bool> selected(p_input.count, false);
	for (uint32_t id : ids) {
		const auto &n = p_full.tree.nodes[id];
		if (n.is_leaf()) {
			for (uint32_t i = 0; i < n.payload_count; ++i) {
				const uint32_t src = p_full.leaf_source_index[n.payload_first + i];
				D2_NEED(src < p_input.count);
				D2_NEED(!selected[src]);
				selected[src] = true;
			}
		}
	}
	Input tile;
	tile.high_count = p_input.sh_high_order_count;
	tile.first_count = p_input.sh_first_order_count;
	std::vector<uint32_t> global_index;
	// Never feed the already importance-sorted leaf payload back into the baker.
	for (uint32_t i = 0; i < p_input.count; ++i) {
		if (selected[i]) {
			global_index.push_back(i);
			tile.gaussians.push_back(p_input.gaussians[i]);
			for (uint32_t c = 0; c < tile.high_count; ++c) {
				tile.sh.push_back(p_input.sh_high_order[uint64_t(i) * tile.high_count + c]);
			}
		}
	}
	D2_NEED(tile.gaussians.size() > p_params.leaf_max_splats);
	D2_NEED(tile.gaussians.size() < p_input.count);
	BakeParams params = p_params;
	params.forced_root = &root.cell;
	BakeResult actual;
	D2_NEED(bake(tile.view(), params, actual));
	check_rule_one(actual);
	CHECK(actual.tree.nodes[0].cell == root.cell);
	CHECK(subtree_bytes(actual, 0u) == subtree_bytes(p_full, p_root));
	// The source permutation must also map back to the full bake's global order.
	uint64_t first_leaf = UINT64_MAX;
	for (uint32_t id : ids) {
		if (p_full.tree.nodes[id].is_leaf()) {
			first_leaf = MIN(first_leaf, p_full.tree.nodes[id].payload_first);
		}
	}
	D2_NEED(first_leaf + actual.leaf_source_index.size() <= p_full.leaf_source_index.size());
	for (uint32_t i = 0; i < actual.leaf_source_index.size(); ++i) {
		const uint32_t local = actual.leaf_source_index[i];
		D2_NEED(local < global_index.size());
		CHECK(global_index[local] == p_full.leaf_source_index[first_leaf + i]);
	}
	if (p_expect_smaller_auto_root) {
		BakeResult automatic;
		D2_NEED(bake(tile.view(), p_params, automatic));
		CHECK_FALSE(automatic.tree.nodes[0].cell == root.cell);
		CHECK(subtree_bytes(automatic, 0u) != subtree_bytes(p_full, p_root));
	}
}

struct CancelFile {
	String path = OS::get_singleton()->get_temp_path().path_join(
			"godotgs_d2_" + itos(OS::get_singleton()->get_process_id()) + "_" +
			itos(OS::get_singleton()->get_ticks_usec()) + ".cancel");
	bool owned = false;
	bool create() {
		if (FileAccess::exists(path)) {
			return false;
		}
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		owned = file.is_valid();
		return owned;
	}
	~CancelFile() {
		if (owned) {
			DirAccess::remove_absolute(path);
		}
	}
};

inline Dictionary limits(const String &p_cancel) {
	Dictionary d;
	d["job_wall_seconds"] = 5400; // Owner Q4 deadline; tests force expiry without sleeping.
	d["batch_wall_seconds"] = 5400;
	d["cancel_and_drain_seconds"] = 5;
	d["process_private_bytes"] = int64_t(2) << 30;
	d["minimum_free_host_physical_and_commit_bytes"] = 1;
	d["minimum_free_output_volume_bytes"] = 1;
	d["max_source_bytes"] = int64_t(1) << 28;
	d["max_input_splats"] = 131072;
	d["max_prepared_splats"] = 262144;
	d["max_nodes"] = 4096;
	d["max_output_bytes"] = int64_t(1) << 28;
	d["max_scratch_bytes"] = int64_t(1) << 30;
	d["concurrent_jobs"] = 1;
	d["pending_jobs"] = 0;
	d["bake_workers"] = 1; // A single bake, not the D2-2 tile scheduler.
	d["image_payload_bytes"] = int64_t(1) << 26;
	d["metric_scratch_peak_bytes"] = int64_t(1) << 28;
	d["node_work"] = int64_t(1) << 28;
	d["bake_work"] = int64_t(1) << 34;
	d["cancel_path"] = p_cancel;
	return d;
}

inline bool offline_bake(OfflineJob &p_job, const BakeInput &p_input,
		const BakeParams &p_params, BakeResult &r_result) {
	OfflineJobScope scope(p_job);
	if (!scope.acquired) {
		FAIL("D2 could not acquire the offline job scope");
		return false;
	}
	return bake(p_input, p_params, r_result);
}

inline void check_refused(OfflineJob &p_job, const BakeInput &p_input,
		const BakeParams &p_params, Error p_expected, BakeResult &r_result) {
	OfflineJobScope scope(p_job);
	D2_NEED(scope.acquired);
	String error;
	Error code = OK;
	CHECK_FALSE(bake_world(p_input, p_params, r_result, &error, &code));
	CHECK(code == p_expected);
	CHECK_FALSE(error.is_empty());
	CHECK(r_result.tree.nodes.is_empty());
	CHECK(r_result.tree.interior_gaussians.is_empty());
	CHECK(r_result.tree.interior_sh_high_order.is_empty());
	CHECK(r_result.leaf_gaussians.is_empty());
	CHECK(r_result.leaf_sh_high_order.is_empty());
	CHECK(r_result.leaf_source_index.is_empty());
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] forced subtrees equal the in-core million-splat world") {
	// Fails if a forced root shrinks, subtree topology/payload changes, or input-index ties change.
	const Input input = synthetic(1048576u);
	BakeParams params;
	BakeResult full;
	D2_NEED(bake(input.view(), params, full));
	check_rule_one(full);
	D2_NEED(full.tree.nodes[0].child_count == 8u);
	const uint32_t first = full.tree.nodes[0].first_child;
	// Two non-contiguous BFS subtrees exercise nonzero leaf offsets and interleaved interiors.
	check_subtree(input.view(), params, full, first, true);
	check_subtree(input.view(), params, full, first + 5u, true);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] forced subtrees equal the in-core scan-like SH3 world") {
	// Fails if SH bytes, tied source order, cell frames, or subtree records differ from the in-core bake.
	const auto hash = [](uint32_t p_index, uint32_t p_channel) {
		uint32_t value = p_index * 0x9e3779b9u + p_channel * 0x85ebca6bu + 0xD2015EEDu;
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		return value ^ (value >> 16u);
	};
	const auto unit = [&](uint32_t p_index, uint32_t p_channel) {
		return float(hash(p_index, p_channel) >> 8u) * (1.0f / 16777216.0f);
	};
	const Quaternion rotations[] = {
		Quaternion(0.6f, 0.0f, 0.0f, 0.8f),
		Quaternion(0.0f, 0.6f, 0.0f, 0.8f),
		Quaternion(0.0f, 0.0f, 0.6f, 0.8f),
		Quaternion(0.5f, 0.5f, 0.5f, 0.5f),
	};
	const Vector3 centers[] = { Vector3(-7, 2, -6), Vector3(6, 4, 7), Vector3(5, 1, -8) };
	Input input;
	input.high_count = 12u;
	input.first_count = 3u;
	input.gaussians.resize(4096u);
	input.sh.resize(input.gaussians.size() * input.high_count);
	for (uint32_t i = 0; i < input.gaussians.size(); ++i) {
		// Each pair duplicates geometry/opacity exactly, hence importance and leaf
		// membership. Distinct SH makes a reversed input-index tie observable in bytes.
		const uint32_t pair = i / 2u;
		const auto u = [&](uint32_t p_channel) { return unit(pair, p_channel); };
		Gaussian &g = input.gaussians[i];
		if (pair % 32u < 20u) {
			// Dense, thin ground surface with triangular rather than uniform density.
			g.position = Vector3(24.0f * (u(0) + u(1) - 1.0f),
					-2.0f + (u(2) - 0.5f) / 64.0f, 20.0f * (u(3) + u(4) - 1.0f));
		} else if (pair % 32u < 31u) {
			// Three compact crowns, interleaved with the ground in source order.
			g.position = centers[hash(pair, 5u) % 3u] + Vector3(
					0.75f * (u(6) + u(7) - 1.0f), 1.25f * (u(8) + u(9) - 1.0f),
					0.5f * (u(10) + u(11) - 1.0f));
		} else {
			// Sparse distant returns force large cells around the much denser clusters.
			g.position = Vector3(128.0f * (u(12) - 0.5f), 32.0f * (u(13) - 0.5f),
					128.0f * (u(14) - 0.5f));
		}
		g.scale = Vector3((1.0f + u(15)) / 32.0f, (1.0f + u(16)) / 512.0f,
				(1.0f + u(17)) / 128.0f);
		g.rotation = rotations[hash(pair, 18u) % 4u];
		g.opacity = pair % 16u == 0u ? 1.0f / 4096.0f :
				(pair % 16u == 1u ? 4095.0f / 4096.0f : 0.125f + 0.75f * u(19));
		g.sh_dc = Color(unit(i, 20u), unit(i, 21u), unit(i, 22u), 1.0f);
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		for (uint32_t c = 0; c < input.first_count + input.high_count; ++c) {
			const Vector3 coefficient((unit(i, 23u + 3u * c) - 0.5f) * 0.5f,
					(unit(i, 24u + 3u * c) - 0.5f) * 0.25f,
					(unit(i, 25u + 3u * c) - 0.5f) * 0.125f);
			if (c < input.first_count) {
				g.sh_1[c] = coefficient;
			} else {
				input.sh[i * input.high_count + c - input.first_count] = coefficient;
			}
		}
	}
	BakeParams params;
	params.leaf_max_splats = 64u; // Several interior levels; offline frozen defaults tested separately.
	BakeResult full;
	D2_NEED(bake(input.view(), params, full));
	check_rule_one(full);
	D2_NEED(full.tree.nodes[0].height >= 3u);
	uint32_t checked = 0;
	for (uint32_t i = 1; i < full.tree.nodes.size() && checked < 2u; ++i) {
		const auto &n = full.tree.nodes[i];
		if (n.height >= 2u && (n.flags & (kNodeFlagSplitByIndex | kNodeFlagOriginCentredRoot)) == 0u) {
			check_subtree(input.view(), params, full, i);
			++checked;
		}
	}
	CHECK(checked == 2u);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] forced root uses half-open centre bounds and rejects invalid cells") {
	// Fails if a fixed negative cell is shrunk, lower faces are excluded, or upper/outside/invalid cells are accepted.
	Input input = synthetic(2u);
	input.gaussians[0].position = Vector3(-4, 0, 0);
	input.gaussians[1].position = Vector3(-0.25f, 3.75f, 3.75f);
	GaussianSplatHlodCell cell{ 2, -1, 0, 0 };
	BakeParams params;
	params.forced_root = &cell;
	BakeResult result;
	D2_NEED(bake(input.view(), params, result));
	CHECK(result.tree.nodes[0].cell == cell);
	CHECK((result.tree.nodes[0].flags & kNodeFlagOriginCentredRoot) == 0u);
	for (int axis = 0; axis < 3; ++axis) {
		const Vector3 saved = input.gaussians[1].position;
		input.gaussians[1].position[axis] = axis == 0 ? 0.0f : 4.0f;
		String error;
		Error code = OK;
		CHECK_FALSE(bake_world(input.view(), params, result, &error, &code));
		CHECK(code == ERR_INVALID_DATA);
		CHECK_FALSE(error.is_empty());
		CHECK(result.tree.nodes.is_empty());
		input.gaussians[1].position = saved;
	}
	for (const GaussianSplatHlodCell invalid : {
			GaussianSplatHlodCell{ kMinCellExponent - 1, -1, 0, 0 },
			GaussianSplatHlodCell{ kMaxCellExponent + 1, -1, 0, 0 },
			GaussianSplatHlodCell{ 2, kMaxCellIndexMagnitude, 0, 0 },
			GaussianSplatHlodCell{ 2, -kMaxCellIndexMagnitude, 0, 0 } }) {
		cell = invalid;
		String error;
		Error code = OK;
		CHECK_FALSE(bake_world(input.view(), params, result, &error, &code));
		CHECK(code == ERR_INVALID_DATA);
		CHECK_FALSE(error.is_empty());
		CHECK(result.tree.nodes.is_empty());
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] MERGE_ONLY offline admission preserves rule-1 bytes") {
	// Fails if MERGE_ONLY is refused, selects instead of merges, changes rule id, or changes the unbounded bake bytes.
	const Input input = synthetic(32768u, 12u);
	BakeParams params;
	CHECK(params.mode == BakeParams::MERGE_ONLY);
	CHECK(params.leaf_max_splats == 16384u);
	CHECK(params.interior.node_max_splats == 16384u);
	CHECK(params.interior.reduction == 4.0);
	CHECK(params.interior.merge_grid == 64u);
	CHECK(params.interior.eps_growth == 1.25);
	CHECK(params.interior.max_eps_steps == 1024u);
	BakeResult reference, bounded;
	D2_NEED(bake(input.view(), params, reference));
	CancelFile cancel;
	D2_NEED(!FileAccess::exists(cancel.path));
	OfflineJob job;
	D2_NEED(job.configure(limits(cancel.path)));
	D2_NEED(job.reserve(input.gaussians.size(), input.high_count, 0u));
	CHECK(job.reserved > 0u);
	D2_NEED(offline_bake(job, input.view(), params, bounded));
	check_rule_one(bounded);
	CHECK(subtree_bytes(reference, 0u) == subtree_bytes(bounded, 0u));
	CHECK(active_offline_job == nullptr);
	CHECK_FALSE(offline_job_busy.load());
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] MERGE_ONLY offline parameters are frozen") {
	// Fails if any non-default leaf/merge parameter is admitted under the frozen rule-1 job contract.
	const Input input = synthetic(32768u);
	CancelFile cancel;
	D2_NEED(!FileAccess::exists(cancel.path));
	OfflineJob job;
	D2_NEED(job.configure(limits(cancel.path)));
	D2_NEED(job.reserve(input.gaussians.size(), 0u, 0u));
	BakeResult accepted;
	D2_NEED(offline_bake(job, input.view(), BakeParams(), accepted));
	for (uint32_t mutation = 0; mutation < 6u; ++mutation) {
		CAPTURE(mutation);
		BakeParams params;
		switch (mutation) {
			case 0: params.leaf_max_splats = 8192u; break;
			case 1: params.interior.node_max_splats = 8192u; break;
			case 2: params.interior.reduction = 2.0; break;
			case 3: params.interior.merge_grid = 32u; break;
			case 4: params.interior.eps_growth = 1.5; break;
			case 5: params.interior.max_eps_steps = 512u; break;
		}
		BakeResult refused = accepted; // A refusal must not expose stale successful output.
		check_refused(job, input.view(), params, ERR_INVALID_DATA, refused);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] offline interiors use SAT rather than prefix selection") {
	// Fails if the interior copies a prefix instead of applying SAT alpha saturation and covariance inflation.
	Input input = synthetic(kMaxNodeSplats + 1u);
	Gaussian identical = input.gaussians[0];
	identical.position = Vector3(1, 1, 1);
	for (Gaussian &g : input.gaussians) {
		g = identical;
	}
	CancelFile cancel;
	D2_NEED(!FileAccess::exists(cancel.path));
	OfflineJob job;
	D2_NEED(job.configure(limits(cancel.path)));
	D2_NEED(job.reserve(input.gaussians.size(), 0u, 0u));
	BakeResult result;
	D2_NEED(offline_bake(job, input.view(), BakeParams(), result));
	check_rule_one(result);
	const auto &root = result.tree.nodes[0];
	D2_NEED(!root.is_leaf() && root.payload_count == 1u);
	const Gaussian &merged = result.tree.interior_gaussians[root.payload_first - result.tree.leaf_splat_count];
	CHECK(merged.opacity == doctest::Approx(0.99f));
	CHECK(merged.opacity != identical.opacity);
	// Coincident identical covariances: SAT multiplies the two largest axes by
	// 1.25; the huge alpha mass saturates at 0.99. Compare covariance, since the
	// eigensolver may permute axes/quaternion representations without changing it.
	double covariance[6];
	splat_covariance(merged, covariance);
	CHECK(covariance[0] == doctest::Approx(std::pow(0.03125 * 1.25, 2)));
	CHECK(covariance[3] == doctest::Approx(std::pow(0.015625 * 1.25, 2)));
	CHECK(covariance[5] == doctest::Approx(std::pow(0.0078125, 2)));
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] MERGE_ONLY RAM reservation fails closed at one byte short") {
	// Fails if insufficient RAM admits input or the exact sufficient reservation cannot run MERGE_ONLY.
	const Input input = synthetic(32768u, 12u);
	CancelFile cancel;
	D2_NEED(!FileAccess::exists(cancel.path));
	OfflineJob sizing;
	D2_NEED(sizing.configure(limits(cancel.path)));
	D2_NEED(sizing.reserve(input.gaussians.size(), input.high_count, 0u));
	D2_NEED(sizing.reserved > 1u);
	Dictionary tight = limits(cancel.path);
	tight["max_scratch_bytes"] = int64_t(sizing.reserved - 1u);
	tight["process_private_bytes"] = int64_t(sizing.reserved - 1u);
	OfflineJob denied;
	D2_NEED(denied.configure(tight));
	CHECK_FALSE(denied.reserve(input.gaussians.size(), input.high_count, 0u));
	CHECK(denied.admitted_count == 0u);
	CHECK(denied.admitted_high == 0u);
	// reserve is the admission boundary before bake allocations, not an allocation fault injection.
	tight["max_scratch_bytes"] = int64_t(sizing.reserved);
	tight["process_private_bytes"] = int64_t(sizing.reserved);
	OfflineJob exact;
	D2_NEED(exact.configure(tight));
	D2_NEED(exact.reserve(input.gaussians.size(), input.high_count, 0u));
	BakeResult accepted;
	D2_NEED(offline_bake(exact, input.view(), BakeParams(), accepted));
	check_rule_one(accepted);
	// Exercise core bake admission too: a false reserve result must not be
	// bypassable just by constructing an OfflineJobScope and calling bake_world.
	check_refused(denied, input.view(), BakeParams(), ERR_OUT_OF_MEMORY, accepted);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] MERGE_ONLY node cap refuses a partial tree") {
	// Fails if the node cap publishes output, returns the wrong error, or blocks a sufficient-cap control.
	const Input input = synthetic(32768u);
	CancelFile cancel;
	D2_NEED(!FileAccess::exists(cancel.path));
	OfflineJob control;
	D2_NEED(control.configure(limits(cancel.path)));
	D2_NEED(control.reserve(input.gaussians.size(), 0u, 0u));
	BakeResult result;
	D2_NEED(offline_bake(control, input.view(), BakeParams(), result));
	D2_NEED(result.tree.nodes.size() > 1u);
	Dictionary low = limits(cancel.path);
	low["max_nodes"] = 1;
	OfflineJob denied;
	D2_NEED(denied.configure(low));
	D2_NEED(denied.reserve(input.gaussians.size(), 0u, 0u));
	check_refused(denied, input.view(), BakeParams(), ERR_OUT_OF_MEMORY, result);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] MERGE_ONLY expired deadline refuses output") {
	// Fails if an expired job succeeds, loses ERR_TIMEOUT, or leaves an earlier result visible.
	const Input input = synthetic(32768u);
	CancelFile cancel;
	D2_NEED(!FileAccess::exists(cancel.path));
	OfflineJob job;
	D2_NEED(job.configure(limits(cancel.path)));
	D2_NEED(job.reserve(input.gaussians.size(), 0u, 0u));
	BakeResult result;
	D2_NEED(offline_bake(job, input.view(), BakeParams(), result));
	job.deadline = 0u; // Deterministic expiry; no wall-clock race or sleep.
	check_refused(job, input.view(), BakeParams(), ERR_TIMEOUT, result);
	CHECK(job.stopped == ERR_TIMEOUT);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] MERGE_ONLY cancellation refuses output and releases scope") {
	// Fails if a real cancellation sentinel is ignored, loses ERR_SKIP, leaks the scope, or exposes stale output.
	const Input input = synthetic(32768u);
	CancelFile cancel;
	D2_NEED(!FileAccess::exists(cancel.path));
	OfflineJob job;
	D2_NEED(job.configure(limits(cancel.path)));
	D2_NEED(job.reserve(input.gaussians.size(), 0u, 0u));
	BakeResult result;
	D2_NEED(offline_bake(job, input.view(), BakeParams(), result));
	D2_NEED(cancel.create());
	check_refused(job, input.view(), BakeParams(), ERR_SKIP, result);
	CHECK(job.stopped == ERR_SKIP);
	CHECK(active_offline_job == nullptr);
	CHECK_FALSE(offline_job_busy.load());
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] two forced-root offline bakes have identical records and payload bytes") {
	// Fails if independent job runs change any node field, Gaussian/SH byte, or source permutation.
	const Input input = synthetic(32768u, 12u);
	GaussianSplatHlodCell root{ 9, 0, 0, 0 };
	BakeParams params;
	params.forced_root = &root;
	CancelFile cancel;
	D2_NEED(!FileAccess::exists(cancel.path));
	BakeResult first, second;
	{
		OfflineJob job;
		D2_NEED(job.configure(limits(cancel.path)));
		D2_NEED(job.reserve(input.gaussians.size(), input.high_count, 0u));
		D2_NEED(offline_bake(job, input.view(), params, first));
	}
	{
		OfflineJob job;
		D2_NEED(job.configure(limits(cancel.path)));
		D2_NEED(job.reserve(input.gaussians.size(), input.high_count, 0u));
		D2_NEED(offline_bake(job, input.view(), params, second));
	}
	check_rule_one(first);
	CHECK(first.tree.nodes[0].cell == root);
	CHECK(subtree_bytes(first, 0u) == subtree_bytes(second, 0u));
	D2_NEED(first.tree.nodes.size() == second.tree.nodes.size());
	for (uint32_t i = 0; i < first.tree.nodes.size(); ++i) {
		CHECK(first.tree.nodes[i].payload_first == second.tree.nodes[i].payload_first);
	}
	D2_NEED(first.tree.interior_gaussians.size() == second.tree.interior_gaussians.size());
	D2_NEED(first.tree.interior_sh_high_order.size() == second.tree.interior_sh_high_order.size());
	CHECK(std::memcmp(first.tree.interior_gaussians.ptr(), second.tree.interior_gaussians.ptr(),
			first.tree.interior_gaussians.size() * sizeof(Gaussian)) == 0);
	CHECK(std::memcmp(first.tree.interior_sh_high_order.ptr(), second.tree.interior_sh_high_order.ptr(),
			first.tree.interior_sh_high_order.size() * sizeof(Vector3)) == 0);
	D2_NEED(first.leaf_source_index.size() == second.leaf_source_index.size());
	CHECK(std::memcmp(first.leaf_source_index.ptr(), second.leaf_source_index.ptr(),
			first.leaf_source_index.size() * sizeof(uint32_t)) == 0);
}

#undef D2_NEED

} // namespace TestHlodD2Bake
