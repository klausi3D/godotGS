#include "hlod_view_set.h"

#include "../core/gs_vector_alloc.h"
#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace gs_hlod_runtime {

namespace {
bool mul_u64(uint64_t p_a, uint64_t p_b, uint64_t &r_out) {
	if (p_a != 0 && p_b > UINT64_MAX / p_a) { return false; }
	r_out = p_a * p_b;
	return true;
}
bool add_u64(uint64_t p_a, uint64_t p_b, uint64_t &r_out) {
	if (p_b > UINT64_MAX - p_a) { return false; }
	r_out = p_a + p_b;
	return true;
}
bool caps_valid(const Caps &p_caps) {
	return p_caps.splat_budget != 0 && p_caps.node_budget != 0 && std::isfinite(p_caps.record_budget) &&
			p_caps.record_budget >= 0.0 && p_caps.record_budget < 9.0e18;
}
// Uniform scale and rotation only: distances scale together with geometric error, so a cut computed in
// the instance frame needs no scale correction. Shear and non-uniform scale are refused.
bool rigid_uniform(const Transform3D &p_transform) {
	if (!p_transform.origin.is_finite()) { return false; }
	const Vector3 c0 = p_transform.basis.get_column(0), c1 = p_transform.basis.get_column(1), c2 = p_transform.basis.get_column(2);
	if (!c0.is_finite() || !c1.is_finite() || !c2.is_finite()) { return false; }
	const double s = c0.length();
	if (!(s > 1.0e-12) || !std::isfinite(s)) { return false; }
	const double tol = 1.0e-4 * s;
	return std::abs(c1.length() - s) <= tol && std::abs(c2.length() - s) <= tol &&
			std::abs(c0.dot(c1)) <= tol * s && std::abs(c0.dot(c2)) <= tol * s && std::abs(c1.dot(c2)) <= tol * s;
}
bool caps_fit(const Caps &p_part, const Caps &p_whole) {
	return p_part.splat_budget <= p_whole.splat_budget && p_part.node_budget <= p_whole.node_budget &&
			p_part.record_budget <= p_whole.record_budget;
}
// Sum of instance caps; false on overflow.
bool sum_instance_caps(const SelectionSnapshot &p_snapshot, Caps &r_sum) {
	r_sum = Caps();
	for (int i = 0; i < p_snapshot.instances.size(); ++i) {
		const Caps &c = p_snapshot.instances[i].caps;
		if (!caps_valid(c) || !add_u64(r_sum.splat_budget, c.splat_budget, r_sum.splat_budget) ||
				c.node_budget > UINT32_MAX - r_sum.node_budget) {
			return false;
		}
		r_sum.node_budget += c.node_budget;
		r_sum.record_budget += c.record_budget;
	}
	return std::isfinite(r_sum.record_budget);
}
} // namespace

bool NodeKey::operator==(const NodeKey &p_other) const {
	return asset_key == p_other.asset_key && cell_e == p_other.cell_e && cell_x == p_other.cell_x &&
			cell_y == p_other.cell_y && cell_z == p_other.cell_z && shape == p_other.shape &&
			split_depth == p_other.split_depth && split_path == p_other.split_path;
}

bool NodeKey::operator<(const NodeKey &p_other) const {
	return std::tie(asset_key, cell_e, cell_x, cell_y, cell_z, shape, split_depth, split_path) <
			std::tie(p_other.asset_key, p_other.cell_e, p_other.cell_x, p_other.cell_y, p_other.cell_z, p_other.shape,
					p_other.split_depth, p_other.split_path);
}

bool make_node_key(const GaussianSplatHlodRuntimeData &p_data, uint32_t p_node, uint64_t p_asset_key, NodeKey &r_key) {
	const uint32_t count = p_data.nodes.size();
	if (p_node >= count) { return false; }
	const auto &top = p_data.nodes[p_node].topology;
	r_key = NodeKey();
	r_key.asset_key = p_asset_key;
	r_key.cell_e = top.cell.e;
	r_key.cell_x = top.cell.ix;
	r_key.cell_y = top.cell.iy;
	r_key.cell_z = top.cell.iz;
	r_key.shape = top.flags & (gs_hlod::kNodeFlagSplitByIndex | gs_hlod::kNodeFlagGroupedLeaf | gs_hlod::kNodeFlagOriginCentredRoot);
	// Walk up through ancestors that share this node's cell (index-split groups): the ordinals
	// within the parents' child ranges are content-determined, unlike the array positions.
	uint32_t cur = p_node;
	while (p_data.nodes[cur].topology.flags & gs_hlod::kNodeFlagSplitByIndex) {
		const auto &n = p_data.nodes[cur].topology;
		if (n.parent == gs_hlod::kNoParent) { break; }
		if (n.parent >= count || n.parent >= cur) { return false; }
		const auto &parent = p_data.nodes[n.parent].topology;
		if (!(parent.cell == n.cell)) { break; }
		if (cur < parent.first_child || cur - parent.first_child >= parent.child_count || r_key.split_depth >= 21) { return false; }
		r_key.split_path |= uint64_t(cur - parent.first_child) << (3 * r_key.split_depth);
		++r_key.split_depth;
		cur = n.parent;
	}
	return true;
}

bool ViewSet::view_cost(const ResidencyBudget &p_budget, const Caps &p_caps, uint64_t &r_bytes) {
	if (!caps_valid(p_caps)) { return false; }
	uint64_t sort = 0, records = 0, draw = 0, sum = 0;
	if (!mul_u64(p_caps.splat_budget, p_budget.sort_bytes_per_splat, sort) ||
			!mul_u64(uint64_t(std::ceil(p_caps.record_budget)), p_budget.record_bytes_per_record, records) ||
			!mul_u64(p_caps.node_budget, p_budget.draw_bytes_per_node, draw) || !add_u64(sort, records, sum) ||
			!add_u64(sum, draw, sum)) {
		return false;
	}
	r_bytes = sum;
	return true;
}

bool ViewSet::validate_snapshot(const SelectionSnapshot &p_snapshot) {
	for (int i = 0; i < p_snapshot.assets.size(); ++i) {
		const SnapshotAsset &a = p_snapshot.assets[i];
		if (a.data.is_null() || a.data->nodes.size() == 0 ||
				(a.resident.size() != 0 && uint32_t(a.resident.size()) != uint32_t(a.data->nodes.size()))) {
			return false;
		}
	}
	for (int i = 0; i < p_snapshot.instances.size(); ++i) {
		const SnapshotInstance &inst = p_snapshot.instances[i];
		if (inst.asset_index >= uint32_t(p_snapshot.assets.size()) || !rigid_uniform(inst.transform) || !caps_valid(inst.caps)) {
			return false;
		}
		for (int j = 0; j < i; ++j) {
			if (p_snapshot.instances[j].instance_key == inst.instance_key) { return false; }
		}
	}
	for (int i = 0; i < p_snapshot.views.size(); ++i) {
		for (int j = 0; j < i; ++j) {
			if (p_snapshot.views[j].view_key == p_snapshot.views[i].view_key) { return false; }
		}
	}
	return true;
}

bool ViewSet::ledger(const SelectionSnapshot &p_snapshot, Ledger &r_ledger) const {
	r_ledger = Ledger();
	if (!validate_snapshot(p_snapshot)) { return false; }
	// Shared payload is charged once per distinct asset key, however many instances use it.
	Vector<uint64_t> seen;
	for (int i = 0; i < p_snapshot.instances.size(); ++i) {
		const SnapshotAsset &a = p_snapshot.assets[p_snapshot.instances[i].asset_index];
		bool duplicate = false;
		for (int j = 0; j < seen.size(); ++j) { duplicate = duplicate || seen[j] == a.asset_key; }
		if (duplicate) { continue; }
		seen.push_back(a.asset_key);
		if (!add_u64(r_ledger.shared_payload_bytes, a.payload_bytes, r_ledger.shared_payload_bytes)) { return false; }
	}
	for (int i = 0; i < views.size(); ++i) {
		if (!add_u64(r_ledger.view_bytes, views[i].reserved_bytes, r_ledger.view_bytes)) { return false; }
	}
	uint64_t total = 0;
	return add_u64(r_ledger.shared_payload_bytes, r_ledger.view_bytes, total);
}

bool ViewSet::prepare_slots(View &r_view, const SelectionSnapshot &p_snapshot) const {
	// Instances in ascending instance key; the slots mirror that order.
	Vector<uint32_t> order;
	for (int i = 0; i < p_snapshot.instances.size(); ++i) { order.push_back(i); }
	std::sort(order.ptrw(), order.ptrw() + order.size(), [&](uint32_t a, uint32_t b) {
		return p_snapshot.instances[a].instance_key < p_snapshot.instances[b].instance_key;
	});
	bool matches = r_view.slots.size() == order.size();
	for (int i = 0; matches && i < order.size(); ++i) {
		const SnapshotInstance &inst = p_snapshot.instances[order[i]];
		const SnapshotAsset &asset = p_snapshot.assets[inst.asset_index];
		matches = r_view.slots[i].instance_key == inst.instance_key &&
				r_view.slots[i].node_count == uint32_t(asset.data->nodes.size());
	}
	if (!matches) {
		Vector<InstanceSlot> next;
		if (!gs_resize_or_fail(next, order.size(), "HLOD view instance slots")) { return false; }
		for (int i = 0; i < order.size(); ++i) {
			const SnapshotInstance &inst = p_snapshot.instances[order[i]];
			const SnapshotAsset &asset = p_snapshot.assets[inst.asset_index];
			const uint32_t nodes = asset.data->nodes.size();
			InstanceSlot &slot = next.write[i];
			slot.instance_key = inst.instance_key;
			slot.node_count = nodes;
			slot.content_revision = asset.data->source_revision;
			bool reused = false;
			for (int j = 0; j < r_view.slots.size(); ++j) {
				if (r_view.slots[j].instance_key == inst.instance_key && r_view.slots[j].node_count == nodes) {
					slot.history = r_view.slots[j].history;
					slot.content_revision = r_view.slots[j].content_revision;
					reused = true;
					break;
				}
			}
			if (!reused && !slot.history.reserve(nodes)) { return false; }
		}
		r_view.slots = std::move(next);
	}
	// A new content revision invalidates the hysteresis of that instance only.
	for (int i = 0; i < order.size(); ++i) {
		const SnapshotAsset &asset = p_snapshot.assets[p_snapshot.instances[order[i]].asset_index];
		InstanceSlot &slot = r_view.slots.write[i];
		if (slot.content_revision != asset.data->source_revision) {
			slot.history.invalidate();
			slot.content_revision = asset.data->source_revision;
		}
	}
	return true;
}

ViewSet::AdmitResult ViewSet::admit_view(const SelectionSnapshot &p_snapshot, const ViewSpec &p_spec) {
	AdmitResult out;
	out.budget_bytes = p_snapshot.budget.physical_bytes;
	if (!validate_snapshot(p_snapshot) || !caps_valid(p_spec.caps) || p_snapshot.instances.is_empty()) { return out; }
	for (int i = 0; i < views.size(); ++i) {
		if (views[i].view_key == p_spec.view_key) { out.status = REFUSED_DUPLICATE; return out; }
	}
	if (uint32_t(views.size()) >= p_snapshot.budget.max_views) { out.status = REFUSED_VIEW_LIMIT; return out; }
	Caps instance_sum;
	if (!sum_instance_caps(p_snapshot, instance_sum) || !caps_fit(instance_sum, p_spec.caps)) {
		out.status = REFUSED_CAPS;
		return out;
	}
	Ledger current;
	uint64_t view_bytes = 0, total = 0;
	if (!ledger(p_snapshot, current) || !view_cost(p_snapshot.budget, p_spec.caps, view_bytes) ||
			!add_u64(current.total(), view_bytes, total)) {
		out.status = REFUSED_BUDGET; // Arithmetic that cannot be proven to fit is a refusal, never a wrap.
		return out;
	}
	out.view_bytes = view_bytes;
	out.charged_bytes = current.total();
	if (total > p_snapshot.budget.physical_bytes) { out.status = REFUSED_BUDGET; return out; }
	// Reserve everything on a local view; the admitted set is only touched on full success.
	View view;
	view.view_key = p_spec.view_key;
	view.reserved = p_spec.caps;
	view.reserved_bytes = view_bytes;
	uint64_t node_total = 0;
	for (int i = 0; i < p_snapshot.instances.size(); ++i) {
		node_total += p_snapshot.assets[p_snapshot.instances[i].asset_index].data->nodes.size();
	}
	const uint64_t draw_capacity = std::min<uint64_t>(p_spec.caps.node_budget, node_total);
	if (!prepare_slots(view, p_snapshot) || !gs_resize_or_fail(view.drawn, draw_capacity, "HLOD view draw records") ||
			!gs_resize_or_fail(view.staging, draw_capacity, "HLOD view draw staging")) {
		out.status = REFUSED_RESOURCES;
		return out;
	}
	int at = 0;
	while (at < views.size() && views[at].view_key < p_spec.view_key) { ++at; }
	views.insert(at, std::move(view));
	out.status = ADMITTED;
	return out;
}

bool ViewSet::remove_view(uint64_t p_view_key) {
	for (int i = 0; i < views.size(); ++i) {
		if (views[i].view_key == p_view_key) {
			views.remove_at(i);
			return true;
		}
	}
	return false;
}

const DrawRecord *ViewSet::drawn(uint64_t p_view_key, uint32_t &r_count) const {
	r_count = 0;
	for (int i = 0; i < views.size(); ++i) {
		if (views[i].view_key == p_view_key) {
			r_count = views[i].drawn_count;
			return views[i].drawn.ptr();
		}
	}
	return nullptr;
}

ViewSet::SnapshotStatus ViewSet::select(const SelectionSnapshot &p_snapshot, Vector<ViewResult> &r_results) {
	r_results.clear();
	Ledger current;
	if (!ledger(p_snapshot, current)) { return SNAPSHOT_INVALID; }
	const bool over_budget = current.total() > p_snapshot.budget.physical_bytes;
	Caps instance_sum;
	const bool instance_caps_ok = sum_instance_caps(p_snapshot, instance_sum);
	for (int v = 0; v < views.size(); ++v) {
		View &view = views.write[v];
		const ViewSpec *spec = nullptr;
		for (int i = 0; i < p_snapshot.views.size(); ++i) {
			if (p_snapshot.views[i].view_key == view.view_key) { spec = &p_snapshot.views[i]; }
		}
		if (!spec) { continue; } // Inactive this frame.
		ViewResult result;
		result.view_key = view.view_key;
		result.draw_generation = view.draw_generation;
		if (over_budget || !instance_caps_ok || !caps_valid(spec->caps) || !caps_fit(spec->caps, view.reserved) ||
				!caps_fit(instance_sum, view.reserved)) {
			// The view asks for more than was reserved: report it; nothing is selected or published.
			result.readiness = NOT_ADMITTED;
			result.reservation_exceeded = true;
			r_results.push_back(result);
			continue;
		}
		if (!prepare_slots(view, p_snapshot)) {
			result.readiness = FAILED;
			r_results.push_back(result);
			continue;
		}
		// Instances in ascending key order == slot order.
		Vector<uint32_t> order;
		for (int i = 0; i < p_snapshot.instances.size(); ++i) { order.push_back(i); }
		std::sort(order.ptrw(), order.ptrw() + order.size(), [&](uint32_t a, uint32_t b) {
			return p_snapshot.instances[a].instance_key < p_snapshot.instances[b].instance_key;
		});
		bool any_failed = false, any_not_admitted = false, any_loading = false, record_failure = false;
		uint32_t staged = 0;
		for (int i = 0; i < order.size(); ++i) {
			const SnapshotInstance &inst = p_snapshot.instances[order[i]];
			const SnapshotAsset &asset = p_snapshot.assets[inst.asset_index];
			InstanceSlot &slot = view.slots.write[i];
			// Camera and frustum in the instance frame (rigid + uniform scale: error and distance scale together).
			const Transform3D inverse = inst.transform.affine_inverse();
			CutRequest request;
			request.camera = inverse.xform(spec->camera);
			request.frustum_enabled = spec->frustum_enabled;
			for (int p = 0; p < 6 && spec->frustum_enabled; ++p) {
				const Vector3 &n = spec->frustum[p].normal;
				const Basis &b = inst.transform.basis;
				Vector3 local(b.get_column(0).dot(n), b.get_column(1).dot(n), b.get_column(2).dot(n));
				double d = spec->frustum[p].d - n.dot(inst.transform.origin);
				const double len = local.length();
				if (!(len > 0.0) || !std::isfinite(len)) { record_failure = true; break; }
				request.frustum[p] = Plane(local / len, d / len);
			}
			request.focal_pixels = spec->focal_pixels;
			request.tau_pixels = spec->tau_pixels;
			request.tile_area = spec->tile_area;
			request.record_budget = inst.caps.record_budget;
			request.splat_budget = inst.caps.splat_budget;
			request.node_budget = std::min<uint32_t>(inst.caps.node_budget, slot.node_count);
			if (asset.resident.size() != 0) {
				request.resident = asset.resident.ptr();
				request.resident_count = asset.resident.size();
			}
			if (record_failure) { any_failed = true; continue; }
			const CutHistory::Result cut = slot.history.select(*asset.data.ptr(), request);
			result.selected_splats += cut.selected_splats;
			result.estimated_records += cut.estimated_records;
			result.budget_bound = result.budget_bound || cut.budget_bound;
			switch (cut.status) {
				case CutHistory::COMPLETE:
				case CutHistory::EMPTY:
					for (uint32_t k = 0; k < cut.selected_count; ++k) {
						if (staged >= uint32_t(view.staging.size())) { any_failed = true; break; }
						DrawRecord &rec = view.staging.write[staged];
						rec.instance_key = inst.instance_key;
						rec.node = slot.history.selected_node(k);
						rec.version = NodeVersion{ asset.data->source_revision, asset.bake_rule_revision };
						if (!make_node_key(*asset.data.ptr(), rec.node, asset.asset_key, rec.key)) { any_failed = true; break; }
						++staged;
					}
					break;
				case CutHistory::UNAVAILABLE:
				case CutHistory::BOOTSTRAP_MISSING_RESIDENCY:
					any_loading = true;
					break;
				case CutHistory::BOOTSTRAP_EXHAUSTED:
				case CutHistory::BOOTSTRAP_INFEASIBLE:
					any_not_admitted = true;
					break;
				case CutHistory::INVALID:
				case CutHistory::BOOTSTRAP_ARITHMETIC:
				default:
					any_failed = true;
					break;
			}
			if (cut.status != CutHistory::COMPLETE && cut.status != CutHistory::EMPTY && result.worst_status == CutHistory::COMPLETE) {
				result.worst_status = cut.status;
			}
		}
		if (any_failed) {
			result.readiness = FAILED;
		} else if (any_not_admitted) {
			result.readiness = NOT_ADMITTED;
		} else if (any_loading) {
			result.readiness = LOADING;
		} else {
			// Complete for every instance: publish one coherent, deterministically ordered draw set.
			std::sort(view.staging.ptrw(), view.staging.ptrw() + staged, [](const DrawRecord &a, const DrawRecord &b) {
				return a.instance_key != b.instance_key ? a.instance_key < b.instance_key
						: (a.key == b.key ? a.node < b.node : a.key < b.key);
			});
			std::swap(view.drawn, view.staging);
			view.drawn_count = staged;
			++view.draw_generation;
			result.readiness = READY;
			result.drawn_count = staged;
			result.draw_generation = view.draw_generation;
		}
		r_results.push_back(result);
	}
	return over_budget ? SNAPSHOT_OVER_BUDGET : SNAPSHOT_OK;
}

} // namespace gs_hlod_runtime
