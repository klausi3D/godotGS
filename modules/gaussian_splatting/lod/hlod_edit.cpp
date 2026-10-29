#include "hlod_edit.h"

#include "../core/gs_vector_alloc.h"
#include <algorithm>

namespace gs_hlod_runtime {

const char *edit_status_name(EditStatus p_status) {
	switch (p_status) {
		case EditStatus::OK: return "ok";
		case EditStatus::INVALID_BASE: return "edit_invalid_base";
		case EditStatus::STALE_REVISION: return "edit_stale_revision";
		case EditStatus::NO_EDITS: return "edit_empty";
		case EditStatus::TOO_MANY_LEAVES: return "edit_too_many_leaves";
		case EditStatus::INVALID_NODE: return "edit_invalid_node";
		case EditStatus::NOT_A_LEAF: return "edit_not_a_leaf";
		case EditStatus::EMPTY_LEAF: return "edit_would_empty_leaf";
		case EditStatus::LEAF_TOO_LARGE: return "edit_leaf_too_large";
		case EditStatus::INVALID_PAYLOAD: return "edit_invalid_payload";
		case EditStatus::CENTER_NOT_EXACT: return "edit_center_not_exact";
		case EditStatus::OVER_CPU_BUDGET: return "edit_over_cpu_budget";
	}
	return "edit_unknown";
}

EditStatus make_edited_snapshot(const Ref<GaussianSplatHlodRuntimeData> &p_base, uint64_t p_expected_revision,
		const LeafEdit *p_edits, uint32_t p_count, Ref<GaussianSplatHlodRuntimeData> &r_snapshot, String &r_reason) {
	auto fail = [&](EditStatus p_status, const String &p_detail) {
		r_reason = String(edit_status_name(p_status)) + (p_detail.is_empty() ? String() : ": " + p_detail);
		return p_status;
	};
	if (p_base.is_null() || p_base->nodes.is_empty() || p_base->edit_revision == UINT64_MAX) {
		return fail(EditStatus::INVALID_BASE, String());
	}
	const GaussianSplatHlodRuntimeData &base = *p_base.ptr();
	if (p_expected_revision != base.edit_revision) {
		return fail(EditStatus::STALE_REVISION, vformat("edited from revision %d, snapshot is at %d", int64_t(p_expected_revision), int64_t(base.edit_revision)));
	}
	if (p_edits == nullptr || p_count == 0) {
		return fail(EditStatus::NO_EDITS, String());
	}
	if (p_count > kMaxEditedLeaves) {
		return fail(EditStatus::TOO_MANY_LEAVES, vformat("%d > %d", int64_t(p_count), int64_t(kMaxEditedLeaves)));
	}
	const uint32_t node_total = base.nodes.size();
	const uint64_t revision = base.edit_revision + 1;
	// Validate everything before building anything: a refused edit allocates nothing that outlives the call.
	for (uint32_t i = 0; i < p_count; ++i) {
		const LeafEdit &edit = p_edits[i];
		if (edit.node >= node_total) {
			return fail(EditStatus::INVALID_NODE, vformat("node %d", int64_t(edit.node)));
		}
		for (uint32_t k = 0; k < i; ++k) {
			if (p_edits[k].node == edit.node) {
				return fail(EditStatus::INVALID_NODE, vformat("node %d edited twice in one transaction", int64_t(edit.node)));
			}
		}
		const auto &node = base.nodes[edit.node];
		if (node.topology.child_count != 0) {
			return fail(EditStatus::NOT_A_LEAF, vformat("node %d", int64_t(edit.node)));
		}
		if (edit.gaussians.is_empty()) {
			return fail(EditStatus::EMPTY_LEAF, vformat("node %d", int64_t(edit.node)));
		}
		if (uint64_t(edit.gaussians.size()) > gs_hlod::kMaxNodeSplats) {
			return fail(EditStatus::LEAF_TOO_LARGE, vformat("node %d has %d splats", int64_t(edit.node), int64_t(edit.gaussians.size())));
		}
		const uint64_t sh_expected = uint64_t(edit.gaussians.size()) * base.high_order_count;
		if (uint64_t(edit.sh.size()) != sh_expected) {
			return fail(EditStatus::INVALID_PAYLOAD, vformat("node %d: %d SH entries, expected %d", int64_t(edit.node), int64_t(edit.sh.size()), int64_t(sh_expected)));
		}
		// The node-relative copy the file format would hold, validated by the strict payload rules (finite fields and
		// the three-sigma support inside the node's AABB and sphere).
		Vector<Gaussian> relative = edit.gaussians;
		for (int64_t j = 0; j < relative.size(); ++j) {
			for (int a = 0; a < 3; ++a) {
				relative.write[j].position[a] = float(double(edit.gaussians[j].position[a]) - node.exact_cell_center[a]);
			}
		}
		GaussianSplatHlodNode shape = node.topology;
		shape.payload_count = uint32_t(relative.size());
		String why;
		if (!gs_world_paged::validate_node_payload(node.exact_cell_center, base.high_order_count, shape, relative.ptr(),
					edit.sh.is_empty() ? nullptr : edit.sh.ptr(), why)) {
			return fail(EditStatus::INVALID_PAYLOAD, vformat("node %d: %s", int64_t(edit.node), why));
		}
		gs_world_paged::NodeMetrics metrics;
		if (!gs_world_paged::compute_node_metrics(edit.gaussians.ptr(), uint32_t(edit.gaussians.size()), node.exact_cell_center, true, metrics, why)) {
			return fail(EditStatus::INVALID_PAYLOAD, vformat("node %d: %s", int64_t(edit.node), why));
		}
		if (!metrics.center_exact) {
			return fail(EditStatus::CENTER_NOT_EXACT, vformat("node %d", int64_t(edit.node)));
		}
	}
	// CPU admission: the overlay of every edited leaf (the new ones plus the ones kept from earlier revisions).
	uint64_t overlay_bytes = 0;
	{
		auto charge = [&](uint64_t p_items, uint64_t p_stride) {
			uint64_t amount;
			if (!allocation_bytes(p_items, p_stride, amount) || amount > GaussianSplatHlodRuntimeData::CPU_BYTE_LIMIT - overlay_bytes) {
				return false;
			}
			overlay_bytes += amount;
			return true;
		};
		for (int64_t i = 0; i < base.edited.size(); ++i) {
			bool replaced = false;
			for (uint32_t k = 0; k < p_count; ++k) {
				replaced = replaced || p_edits[k].node == base.edited[i].node;
			}
			if (!replaced && (!charge(base.edited[i].gaussians.size(), sizeof(Gaussian)) || !charge(base.edited[i].sh.size(), sizeof(Vector3)))) {
				return fail(EditStatus::OVER_CPU_BUDGET, String());
			}
		}
		for (uint32_t k = 0; k < p_count; ++k) {
			if (!charge(p_edits[k].gaussians.size(), sizeof(Gaussian)) || !charge(p_edits[k].sh.size(), sizeof(Vector3))) {
				return fail(EditStatus::OVER_CPU_BUDGET, String());
			}
		}
		if (base.owned_cpu_bytes > GaussianSplatHlodRuntimeData::CPU_BYTE_LIMIT ||
				overlay_bytes > GaussianSplatHlodRuntimeData::CPU_BYTE_LIMIT - base.owned_cpu_bytes) {
			return fail(EditStatus::OVER_CPU_BUDGET, String());
		}
	}

	Ref<GaussianSplatHlodRuntimeData> next;
	next.instantiate();
	next->source_world = base.source_world;
	next->source_data = base.source_data;
	next->source_epoch = base.source_epoch;
	next->source_revision = base.source_revision;
	next->owner_key = base.owner_key;
	next->owned_cpu_bytes = base.owned_cpu_bytes + overlay_bytes; // Conservative: replaced overlays are charged again.
	next->preparation_usec = 0;
	next->leaf_count = base.leaf_count;
	next->parent_count = base.parent_count;
	next->high_order_count = base.high_order_count;
	next->sh_degree = base.sh_degree;
	next->mode_2d = base.mode_2d;
	next->antialiased = base.antialiased;
	next->quantized_eligible = base.quantized_eligible;
	next->leaves = base.leaves;
	next->parents = base.parents;
	next->parent_sh = base.parent_sh;
	next->paged_source = base.paged_source;
	next->paged_sh_first_order = base.paged_sh_first_order;
	next->nodes = base.nodes;
	next->edit_revision = revision;
	next->edit_parent_id = p_base->get_instance_id();
	if (!gs_resize_or_fail(next->edit_state, int64_t(node_total), "HLOD edit state")) {
		return fail(EditStatus::OVER_CPU_BUDGET, String());
	}
	for (uint32_t i = 0; i < node_total; ++i) {
		next->edit_state.write[i] = base.node_edit_state(i);
	}
	// Edited leaves of earlier revisions stay (their overlay is shared copy-on-write), replaced ones are dropped.
	LocalVector<GaussianSplatHlodRuntimeData::EditedLeaf> merged;
	for (int64_t i = 0; i < base.edited.size(); ++i) {
		bool replaced = false;
		for (uint32_t k = 0; k < p_count; ++k) {
			replaced = replaced || p_edits[k].node == base.edited[i].node;
		}
		if (!replaced) {
			merged.push_back(base.edited[i]);
		}
	}
	for (uint32_t k = 0; k < p_count; ++k) {
		const LeafEdit &edit = p_edits[k];
		GaussianSplatHlodRuntimeData::EditedLeaf leaf;
		leaf.node = edit.node;
		leaf.revision = revision;
		const GaussianSplatHlodRuntimeData::EditedLeaf *previous = base.find_edited(edit.node);
		leaf.base_first = previous ? previous->base_first : base.nodes[edit.node].topology.payload_first;
		leaf.base_count = previous ? previous->base_count : base.nodes[edit.node].topology.payload_count;
		leaf.gaussians = edit.gaussians;
		leaf.sh = edit.sh;
		merged.push_back(leaf);
		GaussianSplatHlodRuntimeData::Node &node = next->nodes.write[edit.node];
		gs_world_paged::NodeMetrics metrics;
		String why;
		gs_world_paged::compute_node_metrics(edit.gaussians.ptr(), uint32_t(edit.gaussians.size()), node.exact_cell_center, true, metrics, why);
		node.topology.payload_count = uint32_t(edit.gaussians.size());
		node.footprint = metrics.footprint;
		node.dc_encoding = metrics.dc_encoding;
		node.homogeneous_dc = metrics.homogeneous_dc;
		if (!metrics.homogeneous_dc) {
			next->quantized_eligible = false;
		}
		next->edit_state.write[edit.node] = GaussianSplatHlodRuntimeData::EDIT_LEAF;
		// Every ancestor now represents children it no longer matches.
		for (uint32_t up = node.topology.parent; up != gs_hlod::kNoParent && up < node_total; up = next->nodes[up].topology.parent) {
			if (next->edit_state[up] == GaussianSplatHlodRuntimeData::EDIT_STALE) {
				break;
			}
			next->edit_state.write[up] = GaussianSplatHlodRuntimeData::EDIT_STALE;
		}
	}
	std::sort(merged.ptr(), merged.ptr() + merged.size(), [](const GaussianSplatHlodRuntimeData::EditedLeaf &a, const GaussianSplatHlodRuntimeData::EditedLeaf &b) {
		return a.node < b.node;
	});
	if (!gs_resize_or_fail(next->edited, int64_t(merged.size()), "HLOD edited leaves")) {
		return fail(EditStatus::OVER_CPU_BUDGET, String());
	}
	for (uint32_t i = 0; i < merged.size(); ++i) {
		next->edited.write[i] = merged[i];
	}
	r_snapshot = next;
	r_reason = String();
	return EditStatus::OK;
}

bool collect_edited_since(const GaussianSplatHlodRuntimeData &p_from, const GaussianSplatHlodRuntimeData &p_to,
		LocalVector<uint32_t> &r_nodes) {
	r_nodes.clear();
	if (p_from.nodes.size() != p_to.nodes.size() || p_to.edit_revision <= p_from.edit_revision) {
		return false;
	}
	for (int64_t i = 0; i < p_to.edited.size(); ++i) {
		if (p_to.edited[i].revision > p_from.edit_revision) {
			r_nodes.push_back(p_to.edited[i].node);
		}
	}
	return !r_nodes.is_empty();
}

} // namespace gs_hlod_runtime
