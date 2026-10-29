#include "hlod_view_set.h"

#include "../core/gs_vector_alloc.h"
#include <algorithm>
#include <cmath>
#include <cstring>
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
bool rigid_uniform(const Basis &p_basis) {
	const Vector3 c0 = p_basis.get_column(0), c1 = p_basis.get_column(1), c2 = p_basis.get_column(2);
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
// Sum of instance caps; false on overflow or an invalid cap.
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
uint64_t mix(uint64_t p_hash, uint64_t p_value) {
	return p_hash ^ (p_value + 0x9e3779b97f4a7c15ull + (p_hash << 6) + (p_hash >> 2));
}
uint64_t bits_of(double p_value) {
	uint64_t bits = 0;
	static_assert(sizeof(bits) == sizeof(p_value), "double width");
	memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}
// Everything that, when changed, makes the previous frame's refinement stamps meaningless. Mirrors the
// renderer's policy hash (tau, capacities, record budget) and adds the tile area.
uint64_t policy_hash(const ViewSpec &p_spec, const Caps &p_instance_caps) {
	uint64_t h = 0x484c4f44u;
	h = mix(h, bits_of(p_spec.tau_pixels));
	h = mix(h, bits_of(p_spec.tile_area));
	h = mix(h, p_instance_caps.splat_budget);
	h = mix(h, p_instance_caps.node_budget);
	h = mix(h, bits_of(p_instance_caps.record_budget));
	return h;
}
// Rank of every node's NodeKey (ties by index only when keys are identical, i.e. corrupt input).
bool compute_tie_rank(const GaussianSplatHlodRuntimeData &p_data, uint64_t p_asset_key, Vector<uint32_t> &r_rank) {
	const uint32_t n = p_data.nodes.size();
	Vector<NodeKey> keys;
	Vector<uint32_t> idx, rank;
	if (!gs_resize_or_fail(keys, n, "HLOD node keys") || !gs_resize_or_fail(idx, n, "HLOD node key order") ||
			!gs_resize_or_fail(rank, n, "HLOD node tie ranks")) {
		return false;
	}
	for (uint32_t i = 0; i < n; ++i) {
		if (!make_node_key(p_data, i, p_asset_key, keys.write[i])) { return false; }
		idx.write[i] = i;
	}
	std::sort(idx.ptrw(), idx.ptrw() + n, [&](uint32_t a, uint32_t b) {
		return keys[a] == keys[b] ? a < b : keys[a] < keys[b];
	});
	for (uint32_t r = 0; r < n; ++r) { rank.write[idx[r]] = r; }
	r_rank = std::move(rank);
	return true;
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

uint64_t ViewSet::slot_bytes(uint32_t p_nodes) {
	uint64_t ranks = 0;
	const uint64_t history = CutHistory::workspace_bytes(p_nodes);
	if (history == 0 || !allocation_bytes(p_nodes, sizeof(uint32_t), ranks)) { return 0; }
	uint64_t total = 0;
	return add_u64(history, ranks, total) ? total : 0;
}

bool ViewSet::view_cost(const ResidencyBudget &p_budget, const Caps &p_caps, uint64_t p_history_bytes,
		uint64_t p_draw_capacity, uint64_t &r_bytes) {
	if (!caps_valid(p_caps)) { return false; }
	uint64_t sort = 0, records = 0, draw = 0, storage = 0, sum = 0;
	// Published plus staging draw records and the desired list are all allocated up front.
	if (!mul_u64(p_caps.splat_budget, p_budget.sort_bytes_per_splat, sort) ||
			!mul_u64(uint64_t(std::ceil(p_caps.record_budget)), p_budget.record_bytes_per_record, records) ||
			!mul_u64(p_caps.node_budget, p_budget.draw_bytes_per_node, draw) ||
			!mul_u64(p_draw_capacity, 2 * sizeof(DrawRecord) + sizeof(DesiredRecord), storage) || !add_u64(sort, records, sum) ||
			!add_u64(sum, draw, sum) || !add_u64(sum, storage, sum) || !add_u64(sum, p_history_bytes, sum)) {
		return false;
	}
	r_bytes = sum;
	return true;
}

// A new edit revision (package F1) invalidates a slot's hysteresis exactly like a new source revision does; an
// unedited snapshot keeps its source revision unchanged.
static uint64_t snapshot_content_key(const GaussianSplatHlodRuntimeData &p_data) {
	return p_data.edit_revision == 0 ? p_data.source_revision : (p_data.source_revision * 0x9E3779B97F4A7C15ull) ^ (p_data.edit_revision + 0xD1B54A32D192ED03ull);
}

// Tie ranks are O(n log n) plus an allocation: keep them per asset content (key, snapshot object, revision,
// node count) so admission and re-admission share them. Slots hold a copy-on-write reference.
bool ViewSet::cached_tie_rank(const SnapshotAsset &p_asset, Vector<uint32_t> &r_rank) const {
	const uint64_t source = uint64_t(p_asset.data->get_instance_id());
	const uint64_t revision = p_asset.data->source_revision;
	const uint32_t nodes = p_asset.data->nodes.size();
	for (int i = 0; i < rank_cache.size(); ++i) {
		const RankEntry &e = rank_cache[i];
		if (e.asset_key == p_asset.asset_key && e.source == source && e.revision == revision && e.node_count == nodes) {
			r_rank = e.rank;
			return true;
		}
	}
	RankEntry entry;
	entry.asset_key = p_asset.asset_key;
	entry.source = source;
	entry.revision = revision;
	entry.node_count = nodes;
	if (!compute_tie_rank(*p_asset.data.ptr(), p_asset.asset_key, entry.rank)) { return false; }
	++rank_builds;
	r_rank = entry.rank;
	rank_cache.push_back(entry);
	return true;
}

// Drops cache entries no slot refers to any more (called on admission and removal, never per frame).
void ViewSet::prune_rank_cache() {
	for (int i = rank_cache.size() - 1; i >= 0; --i) {
		bool used = false;
		for (int v = 0; v < views.size() && !used; ++v) {
			for (int k = 0; k < views[v].slots.size(); ++k) {
				if (views[v].slots[k].tie_rank.ptr() == rank_cache[i].rank.ptr()) { used = true; break; }
			}
		}
		if (!used) { rank_cache.remove_at(i); }
	}
}

bool ViewSet::prepare_scratch(const SelectionSnapshot &p_snapshot) const {
	const uint32_t need = std::max<uint32_t>({ uint32_t(p_snapshot.instances.size()), uint32_t(p_snapshot.assets.size()),
			uint32_t(p_snapshot.views.size()), 1u });
	if (uint32_t(order.size()) < need && !gs_resize_or_fail(order, need, "HLOD view order scratch")) { return false; }
	if (uint32_t(index_scratch.size()) < need && !gs_resize_or_fail(index_scratch, need, "HLOD view index scratch")) { return false; }
	if (uint32_t(key_scratch.size()) < need && !gs_resize_or_fail(key_scratch, need, "HLOD view key scratch")) { return false; }
	return true;
}

bool ViewSet::validate_snapshot(const SelectionSnapshot &p_snapshot) const {
	if (!prepare_scratch(p_snapshot)) { return false; }
	for (int i = 0; i < p_snapshot.assets.size(); ++i) {
		const SnapshotAsset &a = p_snapshot.assets[i];
		if (a.data.is_null() || a.data->nodes.size() == 0 ||
				(a.resident.size() != 0 && uint32_t(a.resident.size()) != uint32_t(a.data->nodes.size()))) {
			return false;
		}
	}
	const int instances = p_snapshot.instances.size();
	for (int i = 0; i < instances; ++i) {
		const SnapshotInstance &inst = p_snapshot.instances[i];
		if (inst.asset_index >= uint32_t(p_snapshot.assets.size()) || !rigid_uniform(inst.placement.basis) || !caps_valid(inst.caps)) {
			return false;
		}
		key_scratch.write[i] = inst.instance_key;
	}
	std::sort(key_scratch.ptrw(), key_scratch.ptrw() + instances);
	for (int i = 1; i < instances; ++i) {
		if (key_scratch[i] == key_scratch[i - 1]) { return false; }
	}
	const int view_total = p_snapshot.views.size();
	for (int i = 0; i < view_total; ++i) { key_scratch.write[i] = p_snapshot.views[i].view_key; }
	std::sort(key_scratch.ptrw(), key_scratch.ptrw() + view_total);
	for (int i = 1; i < view_total; ++i) {
		if (key_scratch[i] == key_scratch[i - 1]) { return false; }
	}
	return true;
}

void ViewSet::build_order(const SelectionSnapshot &p_snapshot) const {
	const int n = p_snapshot.instances.size();
	for (int i = 0; i < n; ++i) { order.write[i] = i; }
	std::sort(order.ptrw(), order.ptrw() + n, [&](uint32_t a, uint32_t b) {
		return p_snapshot.instances[a].instance_key < p_snapshot.instances[b].instance_key;
	});
}

bool ViewSet::ledger_impl(const SelectionSnapshot &p_snapshot, Ledger &r_ledger) const {
	r_ledger = Ledger();
	// Shared payload is charged once per distinct asset key, however many instances use it.
	const int n = p_snapshot.instances.size();
	for (int i = 0; i < n; ++i) { index_scratch.write[i] = p_snapshot.instances[i].asset_index; }
	std::sort(index_scratch.ptrw(), index_scratch.ptrw() + n, [&](uint32_t a, uint32_t b) {
		return p_snapshot.assets[a].asset_key < p_snapshot.assets[b].asset_key;
	});
	for (int i = 0; i < n; ++i) {
		const SnapshotAsset &a = p_snapshot.assets[index_scratch[i]];
		if (i > 0 && p_snapshot.assets[index_scratch[i - 1]].asset_key == a.asset_key) { continue; }
		if (!add_u64(r_ledger.shared_payload_bytes, a.payload_bytes, r_ledger.shared_payload_bytes)) { return false; }
	}
	for (int i = 0; i < views.size(); ++i) {
		if (!add_u64(r_ledger.view_bytes, views[i].reserved_bytes, r_ledger.view_bytes)) { return false; }
	}
	uint64_t total = 0;
	return add_u64(r_ledger.shared_payload_bytes, r_ledger.view_bytes, total);
}

bool ViewSet::ledger(const SelectionSnapshot &p_snapshot, Ledger &r_ledger) const {
	r_ledger = Ledger();
	return validate_snapshot(p_snapshot) && ledger_impl(p_snapshot, r_ledger);
}

bool ViewSet::required_storage(const SelectionSnapshot &p_snapshot, uint64_t &r_history_bytes, uint64_t &r_nodes) {
	r_history_bytes = 0;
	r_nodes = 0;
	for (int i = 0; i < p_snapshot.instances.size(); ++i) {
		const uint32_t nodes = p_snapshot.assets[p_snapshot.instances[i].asset_index].data->nodes.size();
		const uint64_t bytes = slot_bytes(nodes);
		if (bytes == 0 || !add_u64(r_history_bytes, bytes, r_history_bytes) || !add_u64(r_nodes, nodes, r_nodes)) { return false; }
	}
	return true;
}

// Requires build_order() for this snapshot. Allocates only when the instance set or a node count changed.
bool ViewSet::prepare_slots(View &r_view, const SelectionSnapshot &p_snapshot) const {
	const int count = p_snapshot.instances.size();
	bool matches = r_view.slots.size() == count;
	for (int i = 0; matches && i < count; ++i) {
		const SnapshotInstance &inst = p_snapshot.instances[order[i]];
		const SnapshotAsset &asset = p_snapshot.assets[inst.asset_index];
		matches = r_view.slots[i].instance_key == inst.instance_key &&
				r_view.slots[i].node_count == uint32_t(asset.data->nodes.size());
	}
	if (!matches) {
		Vector<InstanceSlot> next;
		if (!gs_resize_or_fail(next, count, "HLOD view instance slots")) { return false; }
		for (int i = 0; i < count; ++i) {
			const SnapshotInstance &inst = p_snapshot.instances[order[i]];
			const SnapshotAsset &asset = p_snapshot.assets[inst.asset_index];
			const uint32_t nodes = asset.data->nodes.size();
			InstanceSlot &slot = next.write[i];
			slot.instance_key = inst.instance_key;
			slot.node_count = nodes;
			bool reused = false;
			for (int j = 0; j < r_view.slots.size(); ++j) {
				const InstanceSlot &old = r_view.slots[j];
				if (old.instance_key == inst.instance_key && old.node_count == nodes) {
					slot.history = old.history;
					slot.tie_rank = old.tie_rank;
					slot.content_revision = old.content_revision;
					slot.policy = old.policy;
					reused = true;
					break;
				}
			}
			if (!reused) {
				slot.content_revision = snapshot_content_key(*asset.data.ptr());
				if (!slot.history.reserve(nodes) || !cached_tie_rank(asset, slot.tie_rank)) { return false; }
			}
		}
		r_view.slots = std::move(next);
	}
	// A new content revision invalidates the hysteresis and the key ranks of that instance only.
	for (int i = 0; i < count; ++i) {
		const SnapshotAsset &asset = p_snapshot.assets[p_snapshot.instances[order[i]].asset_index];
		InstanceSlot &slot = r_view.slots.write[i];
		if (slot.content_revision != snapshot_content_key(*asset.data.ptr())) {
			if (!cached_tie_rank(asset, slot.tie_rank)) { return false; }
			slot.history.invalidate();
			slot.content_revision = snapshot_content_key(*asset.data.ptr());
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
	uint64_t history_bytes = 0, node_total = 0;
	if (!required_storage(p_snapshot, history_bytes, node_total)) { out.status = REFUSED_BUDGET; return out; }
	const uint32_t draw_capacity = uint32_t(std::min<uint64_t>(p_spec.caps.node_budget, node_total));
	Ledger current;
	uint64_t view_bytes = 0, total = 0;
	if (!ledger_impl(p_snapshot, current) || !view_cost(p_snapshot.budget, p_spec.caps, history_bytes, draw_capacity, view_bytes) ||
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
	view.reserved_history_bytes = history_bytes;
	view.reserved_draw_capacity = draw_capacity;
	view.origin_revision = p_snapshot.origin_revision;
	build_order(p_snapshot);
	if (!prepare_slots(view, p_snapshot) || !gs_resize_or_fail(view.drawn, draw_capacity, "HLOD view draw records") ||
			!gs_resize_or_fail(view.staging, draw_capacity, "HLOD view draw staging") ||
			!gs_resize_or_fail(view.desired, draw_capacity, "HLOD view desired records") ||
			(uint32_t(result_store.size()) < uint32_t(views.size()) + 1 &&
					!gs_resize_or_fail(result_store, views.size() + 1, "HLOD view results"))) {
		out.status = REFUSED_RESOURCES;
		return out;
	}
	int at = 0;
	while (at < views.size() && views[at].view_key < p_spec.view_key) { ++at; }
	if (views.insert(at, std::move(view)) != OK) {
		out.status = REFUSED_RESOURCES;
		return out;
	}
	prune_rank_cache();
	out.status = ADMITTED;
	return out;
}

bool ViewSet::remove_view(uint64_t p_view_key) {
	for (int i = 0; i < views.size(); ++i) {
		if (views[i].view_key == p_view_key) {
			views.remove_at(i);
			prune_rank_cache();
			return true;
		}
	}
	return false;
}

const DrawRecord *ViewSet::drawn(uint64_t p_view_key, uint32_t &r_count) const {
	r_count = 0;
	for (int i = 0; i < views.size(); ++i) {
		if (views[i].view_key == p_view_key) {
			if (!views[i].ready) { return nullptr; }
			r_count = views[i].drawn_count;
			return views[i].drawn.ptr();
		}
	}
	return nullptr;
}

const DesiredRecord *ViewSet::desired(uint64_t p_view_key, uint32_t &r_count) const {
	r_count = 0;
	for (int i = 0; i < views.size(); ++i) {
		if (views[i].view_key == p_view_key) {
			r_count = views[i].desired_count;
			return r_count ? views[i].desired.ptr() : nullptr;
		}
	}
	return nullptr;
}

uint64_t ViewSet::drawn_origin_revision(uint64_t p_view_key) const {
	for (int i = 0; i < views.size(); ++i) {
		if (views[i].view_key == p_view_key) { return views[i].ready ? views[i].drawn_origin_revision : 0; }
	}
	return 0;
}

uint64_t ViewSet::allocated_bytes() const {
	uint64_t bytes = uint64_t(result_store.size()) * sizeof(ViewResult) + uint64_t(order.size()) * sizeof(uint32_t) +
			uint64_t(index_scratch.size()) * sizeof(uint32_t) + uint64_t(key_scratch.size()) * sizeof(uint64_t);
	for (int c = 0; c < rank_cache.size(); ++c) { bytes += uint64_t(rank_cache[c].rank.size()) * sizeof(uint32_t); }
	for (int v = 0; v < views.size(); ++v) {
		const View &view = views[v];
		bytes += uint64_t(view.drawn.size() + view.staging.size()) * sizeof(DrawRecord) +
				uint64_t(view.desired.size()) * sizeof(DesiredRecord);
		for (int s = 0; s < view.slots.size(); ++s) {
			bytes += view.slots[s].history.allocated_bytes();
		}
	}
	return bytes;
}

ViewSet::SnapshotStatus ViewSet::select(const SelectionSnapshot &p_snapshot) {
	result_count = 0;
	Ledger current;
	if (!validate_snapshot(p_snapshot) || !ledger_impl(p_snapshot, current)) { return SNAPSHOT_INVALID; }
	// An admitted view keeps its reservation while the shared payload plus that reservation still fit the budget.
	// Other views' reservations are not this view's problem: a shrinking headroom only withdraws a view that
	// is genuinely unaffordable, instead of flapping every view whenever the total dips under it.
	bool any_unaffordable = false;
	Caps instance_sum;
	const bool instance_caps_ok = sum_instance_caps(p_snapshot, instance_sum);
	uint64_t needed_history = 0, needed_nodes = 0;
	const bool storage_ok = required_storage(p_snapshot, needed_history, needed_nodes);
	build_order(p_snapshot);
	for (int v = 0; v < views.size(); ++v) {
		View &view = views.write[v];
		const ViewSpec *spec = nullptr;
		for (int i = 0; i < p_snapshot.views.size(); ++i) {
			if (p_snapshot.views[i].view_key == view.view_key) { spec = &p_snapshot.views[i]; }
		}
		if (!spec) { continue; } // Inactive this frame.
		ViewResult &result = result_store.write[result_count++];
		result = ViewResult();
		result.view_key = view.view_key;
		result.draw_generation = view.draw_generation;
		view.ready = false; // Re-established only by a READY outcome below.
		view.desired_count = 0; // Demand is per selection; a view that is not selected has none.
		if (view.origin_revision != p_snapshot.origin_revision) {
			// The frame moved: every history and the published draw set belong to the old offsets. Drop them
			// before anything is selected; the published set is replaced only by a complete selection below.
			for (int s = 0; s < view.slots.size(); ++s) { view.slots.write[s].history.invalidate(); }
			view.drawn_count = 0;
			view.drawn_origin_revision = 0;
			view.origin_revision = p_snapshot.origin_revision;
			result.origin_changed = true;
		}
		result.origin_revision = p_snapshot.origin_revision;
		uint64_t own_cost = 0;
		const bool unaffordable = !add_u64(current.shared_payload_bytes, view.reserved_bytes, own_cost) ||
				own_cost > p_snapshot.budget.physical_bytes;
		any_unaffordable = any_unaffordable || unaffordable;
		if (unaffordable || !instance_caps_ok || !storage_ok || !caps_valid(spec->caps) || !caps_fit(spec->caps, view.reserved) ||
				!caps_fit(instance_sum, view.reserved) || needed_history > view.reserved_history_bytes ||
				std::min<uint64_t>(view.reserved.node_budget, needed_nodes) > view.reserved_draw_capacity) {
			// The snapshot needs more than was reserved (caps, a new instance or larger asset, history or draw
			// storage): explicit, never FAILED. The caller re-admits against the new snapshot.
			result.readiness = NOT_ADMITTED;
			result.reservation_exceeded = true;
			continue;
		}
		if (!prepare_slots(view, p_snapshot)) {
			result.readiness = FAILED;
			continue;
		}
		bool any_failed = false, any_not_admitted = false, any_loading = false;
		uint32_t staged = 0;
		for (int i = 0; i < p_snapshot.instances.size(); ++i) {
			const SnapshotInstance &inst = p_snapshot.instances[order[i]];
			const SnapshotAsset &asset = p_snapshot.assets[inst.asset_index];
			InstanceSlot &slot = view.slots.write[i];
			const uint64_t policy = policy_hash(*spec, inst.caps);
			if (slot.policy != policy) {
				slot.history.invalidate(); // tau, tile area or caps changed: old stamps do not apply.
				slot.policy = policy;
			}
			// Camera and frustum in the instance frame (rigid + uniform scale: error and distance scale together).
			Transform3D frame_transform;
			if (!inst.placement.to_frame_transform(p_snapshot.origin, frame_transform)) { any_failed = true; continue; }
			const Transform3D inverse = frame_transform.affine_inverse();
			CutRequest request;
			request.camera = inverse.xform(spec->camera);
			request.frustum_enabled = spec->frustum_enabled;
			bool bad_frustum = false;
			for (int p = 0; p < 6 && spec->frustum_enabled; ++p) {
				const Vector3 &n = spec->frustum[p].normal;
				const Basis &b = frame_transform.basis;
				Vector3 local(b.get_column(0).dot(n), b.get_column(1).dot(n), b.get_column(2).dot(n));
				double d = spec->frustum[p].d - n.dot(frame_transform.origin);
				const double len = local.length();
				if (!(len > 0.0) || !std::isfinite(len)) { bad_frustum = true; break; }
				request.frustum[p] = Plane(local / len, d / len);
			}
			if (bad_frustum) { any_failed = true; continue; }
			request.focal_pixels = spec->focal_pixels;
			request.tau_pixels = spec->tau_pixels;
			request.tile_area = spec->tile_area;
			request.record_budget = inst.caps.record_budget;
			request.splat_budget = inst.caps.splat_budget;
			request.node_budget = std::min<uint32_t>(inst.caps.node_budget, slot.node_count);
			request.page_splats = spec->page_splats;
			request.page_budget = spec->page_budget;
			request.tie_rank = slot.tie_rank.ptr();
			request.tie_rank_count = slot.tie_rank.size();
			if (asset.resident.size() != 0) {
				request.resident = asset.resident.ptr();
				request.resident_count = asset.resident.size();
			}
			// The camera in the precise frame: origin plus the (float) frame-space camera, added in double.
			gs_world_frame::PreciseCoord camera_precise = spec->camera_precise;
			if (!spec->camera_precise_valid) {
				// Float-only camera: precise only to the float rounding of `camera` (see ViewSpec).
				const double camera_delta[3] = { double(spec->camera.x), double(spec->camera.y), double(spec->camera.z) };
				if (!p_snapshot.origin.translated(camera_delta, camera_precise)) { any_failed = true; continue; }
			}
			const CutHistory::Result cut = slot.history.select(*asset.data.ptr(), request);
			result.selected_splats += cut.selected_splats;
			result.visited_nodes += cut.visited_nodes;
			result.estimated_records += cut.estimated_records;
			result.budget_bound = result.budget_bound || cut.budget_bound;
			for (uint32_t k = 0; k < slot.history.demand_count(); ++k) {
				if (view.desired_count >= uint32_t(view.desired.size())) {
					result.desired_truncated = true;
					// Never leave half a group: drop the copied siblings of the first node that did not fit.
					const uint32_t parent = asset.data->nodes[slot.history.demand_node(k)].topology.parent;
					while (view.desired_count != 0 && !view.desired[view.desired_count - 1].keep &&
							view.desired[view.desired_count - 1].instance_key == inst.instance_key &&
							asset.data->nodes[view.desired[view.desired_count - 1].node].topology.parent == parent) {
						--view.desired_count;
					}
					break;
				}
				DesiredRecord &want = view.desired.write[view.desired_count++];
				want.instance_key = inst.instance_key;
				want.node = slot.history.demand_node(k);
				want.keep = false;
			}
			for (uint32_t k = 0; k < slot.history.partial_frontier_count(); ++k) {
				if (view.desired_count >= uint32_t(view.desired.size())) { result.desired_truncated = true; break; }
				DesiredRecord &want = view.desired.write[view.desired_count++];
				want.instance_key = inst.instance_key;
				want.node = slot.history.partial_frontier_node(k);
				want.keep = true;
			}
			result.desired_count = view.desired_count;
			switch (cut.status) {
				case CutHistory::COMPLETE:
				case CutHistory::EMPTY:
					for (uint32_t k = 0; k < cut.selected_count; ++k) {
						if (staged >= uint32_t(view.staging.size())) { any_failed = true; break; }
						DrawRecord &rec = view.staging.write[staged];
						rec.instance_key = inst.instance_key;
						rec.node = slot.history.selected_node(k);
						rec.version = NodeVersion{ asset.data->node_content_revision(rec.node), asset.bake_rule_revision };
						if (!make_node_key(*asset.data.ptr(), rec.node, asset.asset_key, rec.key) ||
								!gs_world_frame::camera_relative_offset(inst.placement, asset.data->nodes[rec.node].precise_center,
										camera_precise, rec.camera_offset)) {
							any_failed = true;
							break;
						}
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
			// One instance still loading: the whole view is loading and publishes nothing, even if the
			// other instances are complete.
			result.readiness = LOADING;
		} else {
			// Complete for every instance: publish one coherent, deterministically ordered draw set.
			std::sort(view.staging.ptrw(), view.staging.ptrw() + staged, [](const DrawRecord &a, const DrawRecord &b) {
				return a.instance_key != b.instance_key ? a.instance_key < b.instance_key
						: (a.key == b.key ? a.node < b.node : a.key < b.key);
			});
			std::swap(view.drawn, view.staging);
			view.drawn_count = staged;
			view.drawn_origin_revision = p_snapshot.origin_revision;
			++view.draw_generation;
			view.ready = true;
			result.readiness = READY;
			result.drawn_count = staged;
			result.draw_generation = view.draw_generation;
		}
	}
	return any_unaffordable ? SNAPSHOT_OVER_BUDGET : SNAPSHOT_OK;
}

} // namespace gs_hlod_runtime
