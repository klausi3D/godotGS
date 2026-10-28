#include "hlod_matched_arms.h"

#ifdef TESTS_ENABLED
#include "hlod_runtime_cut.h"
#include "../core/gs_vector_alloc.h"
#include "../io/gaussian_splat_world_io.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gs_hlod::matched_arms {
namespace {

uint64_t per_splat_bytes(uint32_t p_high) {
	return sizeof(Gaussian) + uint64_t(p_high) * sizeof(Vector3);
}

// Per-node descendant leaf ranges, validated contiguous (the bake's DFS leaf order).
struct Ranges {
	LocalVector<uint64_t> splat_first, splat_count;
	LocalVector<uint32_t> leaf_first, leaf_count; // ordinals into dfs_leaves
	LocalVector<uint32_t> dfs_leaves;
};

bool build_ranges(const GaussianSplatHlodTree &p_tree, Ranges &r_ranges, String &r_reason) {
	const uint32_t n = p_tree.nodes.size();
	r_ranges.splat_first.resize(n);
	r_ranges.splat_count.resize(n);
	r_ranges.leaf_first.resize(n);
	r_ranges.leaf_count.resize(n);
	r_ranges.dfs_leaves.clear();
	LocalVector<uint32_t> stack;
	stack.push_back(0u);
	while (!stack.is_empty()) {
		const uint32_t id = stack[stack.size() - 1u];
		stack.resize(stack.size() - 1u);
		const GaussianSplatHlodNode &node = p_tree.nodes[id];
		if (node.child_count > 0u) {
			for (uint32_t c = node.child_count; c > 0u; c--) {
				stack.push_back(node.first_child + c - 1u);
			}
			continue;
		}
		r_ranges.leaf_first[id] = r_ranges.dfs_leaves.size();
		r_ranges.leaf_count[id] = 1u;
		r_ranges.splat_first[id] = node.payload_first;
		r_ranges.splat_count[id] = node.payload_count;
		r_ranges.dfs_leaves.push_back(id);
	}
	for (uint32_t i = n; i > 0u; i--) {
		const uint32_t id = i - 1u;
		const GaussianSplatHlodNode &node = p_tree.nodes[id];
		if (node.child_count == 0u) {
			continue;
		}
		const uint32_t first = node.first_child;
		r_ranges.leaf_first[id] = r_ranges.leaf_first[first];
		r_ranges.splat_first[id] = r_ranges.splat_first[first];
		uint32_t leaves = 0u;
		uint64_t splats = 0u;
		for (uint32_t c = 0; c < node.child_count; c++) {
			const uint32_t child = first + c;
			if (r_ranges.leaf_first[child] != r_ranges.leaf_first[id] + leaves ||
					r_ranges.splat_first[child] != r_ranges.splat_first[id] + splats) {
				r_reason = vformat("Descendant leaf ranges of node %d are not contiguous.", id);
				return false;
			}
			leaves += r_ranges.leaf_count[child];
			splats += r_ranges.splat_count[child];
		}
		r_ranges.leaf_count[id] = leaves;
		r_ranges.splat_count[id] = splats;
	}
	return true;
}

template <typename V>
bool fallible_resize(V &r_vec, uint64_t p_count, const char *p_where) {
	if (p_count > uint64_t(UINT32_MAX) ||
			!bake_allocation_probe(p_count * uint64_t(sizeof(*r_vec.ptr())), p_where)) {
		return false;
	}
	r_vec.resize(uint32_t(p_count));
	return true;
}

// JSON-faithful equality: the world stores metadata as JSON, so integers reload as doubles and key
// order is not significant. Everything else (strings, bools, nesting, key sets) must match exactly.
bool metadata_equal(const Variant &p_a, const Variant &p_b) {
	const bool a_num = p_a.get_type() == Variant::INT || p_a.get_type() == Variant::FLOAT;
	const bool b_num = p_b.get_type() == Variant::INT || p_b.get_type() == Variant::FLOAT;
	if (a_num || b_num) {
		return a_num && b_num && double(p_a) == double(p_b);
	}
	if (p_a.get_type() != p_b.get_type()) {
		return false;
	}
	if (p_a.get_type() == Variant::DICTIONARY) {
		const Dictionary a = p_a, b = p_b;
		if (a.size() != b.size()) {
			return false;
		}
		for (const Variant &key : a.get_key_list()) {
			if (!b.has(key) || !metadata_equal(a[key], b[key])) {
				return false;
			}
		}
		return true;
	}
	if (p_a.get_type() == Variant::ARRAY) {
		const Array a = p_a, b = p_b;
		if (a.size() != b.size()) {
			return false;
		}
		for (int i = 0; i < a.size(); i++) {
			if (!metadata_equal(a[i], b[i])) {
				return false;
			}
		}
		return true;
	}
	return p_a == p_b;
}

bool finite3(const Vector3 &p_v) {
	return std::isfinite(p_v.x) && std::isfinite(p_v.y) && std::isfinite(p_v.z);
}

} // namespace

Admission Admission::default_contract() {
	Admission a;
	a.max_total_splats = runtime_cut::MAX_INPUT_SPLATS;
	a.max_nodes = runtime_cut::MAX_NODES;
	a.max_bytes = runtime_cut::MAX_ALLOCATION_BYTES;
	return a;
}

Admission Admission::large_contract() {
	Admission a;
	a.max_total_splats = 8000000u;
	a.max_nodes = 262144u;
	a.max_bytes = uint64_t(12) << 30u;
	return a;
}

const char *status_name(Status p_status) {
	switch (p_status) {
		case Status::OK:
			return "OK";
		case Status::INVALID_INPUT:
			return "INVALID_INPUT";
		case Status::ADMISSION_REFUSED:
			return "ADMISSION_REFUSED";
		case Status::INVALID_CUT:
			return "INVALID_CUT";
		case Status::CAPTURE_MISMATCH:
			return "CAPTURE_MISMATCH";
		case Status::COUNT_MISMATCH:
			return "COUNT_MISMATCH";
		case Status::REFERENCE_MISMATCH:
			return "REFERENCE_MISMATCH";
		case Status::ALLOCATION_FAILED:
			return "ALLOCATION_FAILED";
		case Status::SERIALIZATION_FAILED:
			return "SERIALIZATION_FAILED";
		case Status::ROUNDTRIP_MISMATCH:
			return "ROUNDTRIP_MISMATCH";
	}
	return "UNKNOWN";
}

Status admit(const Admission &p_contract, uint64_t p_leaf_splats, uint64_t p_interior_splats, uint64_t p_nodes,
		uint32_t p_sh_high_order_count, uint64_t p_cut_splats, uint64_t p_capture_bytes, String &r_reason) {
	if (p_contract.max_total_splats == 0u || p_contract.max_nodes == 0u || p_contract.max_bytes == 0u) {
		r_reason = "Admission contract is not predeclared (a zero limit).";
		return Status::ADMISSION_REFUSED;
	}
	if (p_leaf_splats > p_contract.max_total_splats || p_interior_splats > p_contract.max_total_splats - p_leaf_splats) {
		r_reason = vformat("Leaves %d + interiors %d exceed the %d-splat contract.", p_leaf_splats, p_interior_splats,
				p_contract.max_total_splats);
		return Status::ADMISSION_REFUSED;
	}
	if (p_nodes > p_contract.max_nodes) {
		r_reason = vformat("%d nodes exceed the %d-node contract.", p_nodes, p_contract.max_nodes);
		return Status::ADMISSION_REFUSED;
	}
	// Arms: three reduced sets of p_cut_splats plus the all-leaf reference; capture already held.
	const uint64_t per = per_splat_bytes(p_sh_high_order_count);
	if (p_cut_splats > p_contract.max_total_splats) {
		r_reason = vformat("Cut of %d splats exceeds the %d-splat contract.", p_cut_splats, p_contract.max_total_splats);
		return Status::ADMISSION_REFUSED;
	}
	const uint64_t splats = 3u * p_cut_splats + p_leaf_splats; // each term <= 8M-class: cannot overflow
	const uint64_t bytes = splats * per;
	const uint64_t fixed = p_nodes * 1024u + p_capture_bytes;
	if (bytes > UINT64_MAX - fixed || bytes + fixed > p_contract.max_bytes) {
		r_reason = vformat("Estimated %d bytes exceed the %d-byte contract.", bytes + fixed, p_contract.max_bytes);
		return Status::ADMISSION_REFUSED;
	}
	return Status::OK;
}

Status select_cut(const GaussianSplatHlodTree &p_tree, const View &p_view, LocalVector<uint32_t> &r_cut, String &r_reason) {
	r_cut.clear();
	const double norm = std::sqrt(double(p_view.forward.x) * p_view.forward.x + double(p_view.forward.y) * p_view.forward.y +
			double(p_view.forward.z) * p_view.forward.z);
	if (p_tree.nodes.is_empty() || !finite3(p_view.position) || !finite3(p_view.forward) || !std::isfinite(norm) || norm == 0.0 ||
			!std::isfinite(p_view.focal_pixels) || p_view.focal_pixels <= 0.0 || !std::isfinite(p_view.tau_pixels) ||
			p_view.tau_pixels < 0.0 || !std::isfinite(p_view.near_plane) || p_view.near_plane <= 0.0) {
		r_reason = "Invalid view or empty tree.";
		return Status::INVALID_INPUT;
	}
	double dir[3];
	for (int a = 0; a < 3; a++) {
		dir[a] = double(p_view.forward[a]) / norm;
	}
	LocalVector<uint32_t> stack;
	stack.push_back(0u);
	while (!stack.is_empty()) {
		const uint32_t id = stack[stack.size() - 1u];
		stack.resize(stack.size() - 1u);
		const GaussianSplatHlodNode &node = p_tree.nodes[id];
		bool accept = node.is_leaf();
		if (!accept) {
			double center[3];
			p_tree.node_cell_center(node, center);
			double depth = 0.0;
			for (int a = 0; a < 3; a++) {
				const double lo = center[a] + double(node.aabb_min[a]);
				const double hi = center[a] + double(node.aabb_max[a]);
				depth += ((dir[a] >= 0.0 ? lo : hi) - double(p_view.position[a])) * dir[a];
			}
			if (std::isfinite(depth) && depth > p_view.near_plane) {
				const double pixels = double(node.geometric_error) / depth * p_view.focal_pixels;
				accept = std::isfinite(pixels) && pixels <= p_view.tau_pixels;
			}
		}
		if (accept) {
			r_cut.push_back(id);
			continue;
		}
		for (uint32_t c = node.child_count; c > 0u; c--) {
			stack.push_back(node.first_child + c - 1u);
		}
	}
	return Status::OK;
}

Status cut_by_height(const GaussianSplatHlodTree &p_tree, uint32_t p_height, LocalVector<uint32_t> &r_cut, String &r_reason) {
	r_cut.clear();
	if (p_tree.nodes.is_empty()) {
		r_reason = "Empty tree.";
		return Status::INVALID_INPUT;
	}
	LocalVector<uint32_t> stack;
	stack.push_back(0u);
	while (!stack.is_empty()) {
		const uint32_t id = stack[stack.size() - 1u];
		stack.resize(stack.size() - 1u);
		const GaussianSplatHlodNode &node = p_tree.nodes[id];
		if (node.height <= p_height || node.is_leaf()) {
			r_cut.push_back(id);
			continue;
		}
		for (uint32_t c = node.child_count; c > 0u; c--) {
			stack.push_back(node.first_child + c - 1u);
		}
	}
	return Status::OK;
}

static Status assemble_impl(const BakeInput &p_input, const BakeResult &p_bake, const CandidateCapture &p_capture,
		const LocalVector<uint32_t> &p_cut, const Admission &p_contract, ArmSet &r_arms, String &r_reason) {
	r_arms = ArmSet();
	const GaussianSplatHlodTree &tree = p_bake.tree;
	const uint32_t high = tree.sh_high_order_count;
	const uint32_t n = tree.leaf_splat_count;
	if (tree.nodes.is_empty() || tree.bake_rule_version != kQualityBakeRuleVersion || !tree.interior_resident ||
			p_capture.nodes.size() != tree.nodes.size() || p_input.count != n || p_input.sh_high_order_count != high ||
			p_bake.leaf_gaussians.size() != n || p_bake.leaf_source_index.size() != n ||
			p_bake.leaf_sh_high_order.size() != uint64_t(n) * high || p_cut.is_empty()) {
		r_reason = "Inputs do not describe one quality bake with a capture and a non-empty cut.";
		return Status::INVALID_INPUT;
	}
	String reason;
	Status status = admit(p_contract, n, tree.interior_splat_count, tree.nodes.size(), high, 0u, p_capture.captured_bytes, reason);
	if (status != Status::OK) {
		r_reason = reason;
		return status;
	}
	Ranges ranges;
	if (!build_ranges(tree, ranges, reason)) {
		r_reason = reason;
		return Status::INVALID_CUT;
	}
	// Order the cut by its descendant leaf range and require an exact tiling of the whole domain.
	LocalVector<uint32_t> cut = p_cut;
	for (uint32_t id : cut) {
		if (id >= tree.nodes.size()) {
			r_reason = vformat("Cut node %d is out of range.", id);
			return Status::INVALID_CUT;
		}
	}
	std::sort(cut.ptr(), cut.ptr() + cut.size(), [&](uint32_t a, uint32_t b) {
		return ranges.splat_first[a] < ranges.splat_first[b] || (ranges.splat_first[a] == ranges.splat_first[b] && a < b);
	});
	uint64_t cursor = 0u;
	uint64_t cut_splats = 0u;
	for (uint32_t id : cut) {
		if (ranges.splat_first[id] != cursor) {
			r_reason = vformat("Cut is not a complete disjoint cut: node %d starts at leaf splat %d, expected %d.", id,
					ranges.splat_first[id], cursor);
			return Status::INVALID_CUT;
		}
		cursor += ranges.splat_count[id];
		cut_splats += tree.nodes[id].payload_count;
	}
	if (cursor != n) {
		r_reason = vformat("Cut covers %d of %d leaf splats.", cursor, n);
		return Status::INVALID_CUT;
	}
	status = admit(p_contract, n, tree.interior_splat_count, tree.nodes.size(), high, cut_splats, p_capture.captured_bytes, reason);
	if (status != Status::OK) {
		r_reason = reason;
		return status;
	}
	DomainReport &domain = r_arms.domain;
	domain.domain_splats = n;
	domain.cut_splats = cut_splats;
	auto reserve = [&](ArmPayload &r_arm) {
		return fallible_resize(r_arm.gaussians, cut_splats, "matched arm gaussians") &&
				(high == 0u || fallible_resize(r_arm.sh_high_order, cut_splats * high, "matched arm sh"));
	};
	if (!reserve(r_arms.sat) || !reserve(r_arms.area) || !reserve(r_arms.s1b)) {
		r_reason = "Cannot allocate the arm payloads.";
		return Status::ALLOCATION_FAILED;
	}
	// DC tags present in the source; no arm may invent one.
	bool dc_present[4] = {};
	for (uint32_t i = 0; i < n; i++) {
		dc_present[uint32_t(gaussian_get_dc_encoding(p_input.gaussians[i].render_meta)) & 3u] = true;
	}
	uint64_t out = 0u;
	for (uint32_t id : cut) {
		const GaussianSplatHlodNode &node = tree.nodes[id];
		CutNodeRecord record;
		record.node = id;
		record.is_leaf = node.is_leaf();
		record.candidate_count = node.payload_count;
		record.leaf_splat_first = ranges.splat_first[id];
		record.leaf_splat_count = ranges.splat_count[id];
		record.leaf_node_first = domain.leaf_node_ids.size();
		record.leaf_node_count = ranges.leaf_count[id];
		for (uint32_t k = 0; k < record.leaf_node_count; k++) {
			domain.leaf_node_ids.push_back(ranges.dfs_leaves[ranges.leaf_first[id] + k]);
		}
		const Gaussian *sat_g, *area_g, *win_g;
		const Vector3 *sat_sh = nullptr, *area_sh = nullptr, *win_sh = nullptr;
		double center[3] = { 0.0, 0.0, 0.0 };
		if (node.is_leaf()) {
			sat_g = area_g = win_g = p_bake.leaf_gaussians.ptr() + node.payload_first;
			if (high) {
				sat_sh = area_sh = win_sh = p_bake.leaf_sh_high_order.ptr() + node.payload_first * high;
			}
			domain.kinds[0]++;
		} else {
			const CandidateCapture::Node &cap = p_capture.nodes[id];
			if (!cap.captured) {
				r_reason = vformat("Interior cut node %d has no captured candidates.", id);
				return Status::CAPTURE_MISMATCH;
			}
			if (cap.sat.size() != node.payload_count || cap.area.size() != node.payload_count) {
				r_reason = vformat("Node %d candidate counts differ: SAT %d, A-area %d, baked %d.", id, cap.sat.size(),
						cap.area.size(), node.payload_count);
				return Status::COUNT_MISMATCH;
			}
			if (cap.sat.sh_high_order.size() != uint64_t(node.payload_count) * high ||
					uint64_t(cap.area.sh.size()) != uint64_t(node.payload_count) * high) {
				r_reason = vformat("Node %d candidate SH layout mismatch.", id);
				return Status::CAPTURE_MISMATCH;
			}
			if (((node.flags & kNodeFlagKindMask) == NODE_KIND_MERGED) != cap.use_merged) {
				r_reason = vformat("Node %d captured winner disagrees with the baked kind.", id);
				return Status::CAPTURE_MISMATCH;
			}
			sat_g = cap.sat.gaussians.ptr();
			area_g = cap.area.gaussians.ptr();
			sat_sh = high ? cap.sat.sh_high_order.ptr() : nullptr;
			area_sh = high ? cap.area.sh.ptr() : nullptr;
			win_g = cap.use_merged ? sat_g : area_g;
			win_sh = cap.use_merged ? sat_sh : area_sh;
			// The winner must be byte-identical to what the baker stored in the tree.
			const uint64_t off = node.payload_first - n;
			if (off + node.payload_count > tree.interior_gaussians.size() ||
					memcmp(win_g, tree.interior_gaussians.ptr() + off, sizeof(Gaussian) * node.payload_count) != 0 ||
					(high && memcmp(win_sh, tree.interior_sh_high_order.ptr() + off * high,
							 sizeof(Vector3) * uint64_t(node.payload_count) * high) != 0)) {
				r_reason = vformat("Node %d captured winner differs from the baked interior payload.", id);
				return Status::CAPTURE_MISMATCH;
			}
			record.use_merged = cap.use_merged;
			domain.kinds[cap.use_merged ? 1 : 2]++;
			tree.node_cell_center(node, center);
		}
		for (uint32_t k = 0; k < node.payload_count; k++, out++) {
			const Gaussian *src[3] = { sat_g + k, area_g + k, win_g + k };
			Gaussian *dst[3] = { &r_arms.sat.gaussians[out], &r_arms.area.gaussians[out], &r_arms.s1b.gaussians[out] };
			for (int arm = 0; arm < 3; arm++) {
				*dst[arm] = *src[arm];
				if (!node.is_leaf()) {
					for (int a = 0; a < 3; a++) {
						const double absolute = center[a] + double(src[arm]->position[a]);
						if (!std::isfinite(absolute)) {
							r_reason = vformat("Node %d position conversion is not finite.", id);
							return Status::INVALID_INPUT;
						}
						dst[arm]->position[a] = real_t(absolute);
					}
				}
				if (!dc_present[uint32_t(gaussian_get_dc_encoding(dst[arm]->render_meta)) & 3u]) {
					r_reason = vformat("Node %d arm %d carries a DC tag absent from the source.", id, arm);
					return Status::CAPTURE_MISMATCH;
				}
			}
			if (high) {
				const Vector3 *shs[3] = { sat_sh, area_sh, win_sh };
				ArmPayload *arms[3] = { &r_arms.sat, &r_arms.area, &r_arms.s1b };
				for (int arm = 0; arm < 3; arm++) {
					memcpy(arms[arm]->sh_high_order.ptr() + out * high, shs[arm] + uint64_t(k) * high, sizeof(Vector3) * high);
				}
			}
		}
		domain.nodes.push_back(record);
	}
	if (out != cut_splats || r_arms.sat.gaussians.size() != cut_splats || r_arms.area.gaussians.size() != cut_splats ||
			r_arms.s1b.gaussians.size() != cut_splats) {
		r_reason = "Arm counts differ from the common cut count.";
		return Status::COUNT_MISMATCH;
	}
	// Independent all-leaf reference: rebuilt from the bake input, not from the baker's leaf copies.
	LocalVector<uint8_t> seen;
	seen.resize(n);
	memset(seen.ptr(), 0, n);
	if (!fallible_resize(r_arms.reference.gaussians, n, "matched reference gaussians") ||
			(high && !fallible_resize(r_arms.reference.sh_high_order, uint64_t(n) * high, "matched reference sh"))) {
		r_reason = "Cannot allocate the reference.";
		return Status::ALLOCATION_FAILED;
	}
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t src = p_bake.leaf_source_index[i];
		if (src >= n || seen[src]) {
			r_reason = "Leaf order is not a permutation of the input.";
			return Status::REFERENCE_MISMATCH;
		}
		seen[src] = 1;
		r_arms.reference.gaussians[i] = p_input.gaussians[src];
		if (high) {
			memcpy(r_arms.reference.sh_high_order.ptr() + uint64_t(i) * high, p_input.sh_high_order + uint64_t(src) * high,
					sizeof(Vector3) * high);
		}
	}
	if (memcmp(r_arms.reference.gaussians.ptr(), p_bake.leaf_gaussians.ptr(), sizeof(Gaussian) * n) != 0 ||
			(high && memcmp(r_arms.reference.sh_high_order.ptr(), p_bake.leaf_sh_high_order.ptr(),
							 sizeof(Vector3) * uint64_t(n) * high) != 0)) {
		r_reason = "All-leaf reference differs from the baked leaf payload.";
		return Status::REFERENCE_MISMATCH;
	}
	return Status::OK;
}

Status assemble(const BakeInput &p_input, const BakeResult &p_bake, const CandidateCapture &p_capture,
		const LocalVector<uint32_t> &p_cut, const Admission &p_contract, ArmSet &r_arms, String &r_reason) {
	const Status status = assemble_impl(p_input, p_bake, p_capture, p_cut, p_contract, r_arms, r_reason);
	if (status != Status::OK) {
		r_arms = ArmSet(); // never leave a partial arm set behind a failure
	}
	return status;
}

Status write_arm(const ArmPayload &p_arm, const ArmMeta &p_meta, const String &p_path, String &r_reason) {
	if (p_arm.gaussians.is_empty() ||
			p_arm.sh_high_order.size() != uint64_t(p_arm.gaussians.size()) * p_meta.sh_high_order_count) {
		r_reason = "Arm payload is empty or its SH layout does not match the metadata.";
		return Status::INVALID_INPUT;
	}
	if (p_meta.antialiased) {
		// The .gsplatworld format carries no antialiased flag (io/gaussian_splat_world_io.cpp: payloads load
		// as classic). Refuse rather than silently export an arm that reloads with different semantics.
		r_reason = "The world format cannot represent antialiased payloads; refusing a silent flag drop.";
		return Status::INVALID_INPUT;
	}
	Ref<GaussianData> data;
	data.instantiate();
	data->set_gaussian_payload(p_arm.gaussians, p_arm.sh_high_order, p_meta.sh_first_order_count, p_meta.sh_high_order_count,
			p_meta.is_2d, p_meta.antialiased);
	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_gaussian_data(data);
	world->set_bounds(data->get_aabb());
	world->set_metadata(p_meta.metadata);
	ResourceFormatSaverGaussianSplatWorld saver;
	const Error err = saver.save_resident_uncompressed(world, p_path);
	if (err != OK) {
		r_reason = vformat("save_resident_uncompressed failed (%d) for %s.", int(err), p_path);
		return Status::SERIALIZATION_FAILED;
	}
	return Status::OK;
}

Status verify_roundtrip(const ArmPayload &p_arm, const ArmMeta &p_meta, const String &p_path, String &r_reason) {
	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = ERR_BUG;
	Ref<GaussianSplatWorld> loaded = loader.load(p_path, "", &err);
	if (err != OK || loaded.is_null()) {
		r_reason = vformat("Reload failed (%d) for %s.", int(err), p_path);
		return Status::SERIALIZATION_FAILED;
	}
	if (loaded->get_gaussian_data().is_null() && loaded->materialize_resident_gaussian_data() != OK) {
		r_reason = "Reloaded world has no resident payload.";
		return Status::ROUNDTRIP_MISMATCH;
	}
	GaussianData::SaveSnapshot snapshot;
	if (loaded->get_gaussian_data().is_null() || loaded->get_gaussian_data()->capture_save_snapshot(snapshot) != OK) {
		r_reason = "Cannot snapshot the reloaded payload.";
		return Status::ROUNDTRIP_MISMATCH;
	}
	const uint32_t count = p_arm.gaussians.size();
	if (snapshot.get_gaussians().size() != int64_t(count) || snapshot.get_sh_first_order_count() != p_meta.sh_first_order_count ||
			snapshot.get_sh_high_order_count() != p_meta.sh_high_order_count || snapshot.get_2d_mode() != p_meta.is_2d ||
			snapshot.get_antialiased() != p_meta.antialiased || loaded->get_sh_degree() != snapshot.get_sh_degree() ||
			loaded->get_antialiased() != p_meta.antialiased || loaded->get_2d_mode() != p_meta.is_2d ||
			loaded->get_splat_count() != count) {
		r_reason = vformat("Reloaded counts, SH layout, 2D or antialiased flag differ (count %d/%d, first %d/%d, high %d/%d, 2d %d/%d, aa %d/%d, world aa %d, world 2d %d, world count %d).",
				int64_t(snapshot.get_gaussians().size()), int64_t(count), snapshot.get_sh_first_order_count(), p_meta.sh_first_order_count,
				snapshot.get_sh_high_order_count(), p_meta.sh_high_order_count, int(snapshot.get_2d_mode()), int(p_meta.is_2d),
				int(snapshot.get_antialiased()), int(p_meta.antialiased), int(loaded->get_antialiased()), int(loaded->get_2d_mode()),
				loaded->get_splat_count());
		return Status::ROUNDTRIP_MISMATCH;
	}
	if (memcmp(snapshot.get_gaussians().ptr(), p_arm.gaussians.ptr(), sizeof(Gaussian) * count) != 0) {
		r_reason = "Reloaded Gaussian payload is not bit-identical.";
		return Status::ROUNDTRIP_MISMATCH;
	}
	if (p_meta.sh_high_order_count &&
			(snapshot.get_sh_high_order().size() != int64_t(count) * p_meta.sh_high_order_count ||
					memcmp(snapshot.get_sh_high_order().ptr(), p_arm.sh_high_order.ptr(),
							sizeof(Vector3) * uint64_t(count) * p_meta.sh_high_order_count) != 0)) {
		r_reason = "Reloaded SH payload is not bit-identical.";
		return Status::ROUNDTRIP_MISMATCH;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (gaussian_get_dc_encoding(snapshot.get_gaussians()[i].render_meta) !=
				gaussian_get_dc_encoding(p_arm.gaussians[i].render_meta)) {
			r_reason = vformat("DC tag of splat %d changed.", i);
			return Status::ROUNDTRIP_MISMATCH;
		}
	}
	if (!metadata_equal(loaded->get_metadata(), p_meta.metadata)) {
		r_reason = vformat("Reloaded metadata differs: %s vs %s.", Variant(loaded->get_metadata()).stringify(), Variant(p_meta.metadata).stringify());
		return Status::ROUNDTRIP_MISMATCH;
	}
	return Status::OK;
}

} // namespace gs_hlod::matched_arms
#endif
