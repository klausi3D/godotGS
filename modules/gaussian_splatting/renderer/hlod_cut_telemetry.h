#ifndef GS_HLOD_CUT_TELEMETRY_H
#define GS_HLOD_CUT_TELEMETRY_H

#include "../lod/hlod_view_set.h"
#include "core/variant/variant.h"

// Render-thread-owned, report-only snapshot of the same selection used for the world draw list.
// The selector's diagnostic scratch is preallocated. Packed exports use COW when readers retain stats.
struct HlodCutTelemetry {
	bool valid = false;
	PackedInt32Array drawn_ids;
	String drawn_digest;
	PackedInt32Array hysteresis_held;
	PackedInt32Array residency_missed;
	Array residency_missed_demanded;
	bool budget_bound = false;
	int32_t first_cap_refused_node = -1;

	void append_stats(Dictionary &r_stats, bool p_drawn_valid) const {
		const bool drawn_valid = valid && p_drawn_valid;
		r_stats["hlod_drawn_node_ids"] = drawn_valid ? Variant(drawn_ids) : Variant();
		r_stats["hlod_drawn_node_digest"] = drawn_valid ? Variant(drawn_digest) : Variant();
		r_stats["hlod_cut_hysteresis_held"] = valid ? Variant(hysteresis_held) : Variant();
		r_stats["hlod_cut_budget_bound"] = valid ? Variant(budget_bound) : Variant();
		r_stats["hlod_cut_first_cap_refused_node"] = valid ? Variant(first_cap_refused_node) : Variant();
		r_stats["hlod_cut_residency_missed"] = valid ? Variant(residency_missed) : Variant();
		// Array has reference semantics: a retained stats snapshot must survive the next capture.
		r_stats["hlod_cut_residency_missed_demanded"] = valid ? Variant(residency_missed_demanded.duplicate()) : Variant();
	}

	void capture(const gs_hlod_runtime::CutHistory &p_history, const gs_hlod_runtime::CutHistory::Result &p_cut,
			uint32_t p_nodes, const gs_hlod_runtime::DrawRecord *p_records, uint32_t p_count) {
		drawn_ids.resize(p_count);
		for (uint32_t i = 0; i < p_count; ++i) { drawn_ids.set(i, int32_t(p_records[i].node)); }
		drawn_ids.sort();
		// FNV-1a over sorted uint32 IDs encoded little-endian, independent of host byte order.
		uint64_t digest = UINT64_C(14695981039346656037);
		for (int32_t id : drawn_ids) {
			for (uint32_t shift = 0; shift < 32; shift += 8) {
				digest ^= (uint32_t(id) >> shift) & 0xffu;
				digest *= UINT64_C(1099511628211);
			}
		}
		drawn_digest = String::num_uint64(digest, 16).pad_zeros(16);
		hysteresis_held.resize(p_cut.hysteresis_held_count);
		residency_missed.resize(p_cut.residency_missed_count);
		residency_missed_demanded.resize(p_cut.residency_missed_count);
		uint32_t held = 0, missed = 0;
		for (uint32_t node = 0; node < p_nodes; ++node) {
			const uint8_t flags = p_history.diagnostic_flags(node);
			if (flags & gs_hlod_runtime::CutHistory::HYSTERESIS_HELD) { hysteresis_held.set(held++, int32_t(node)); }
			if (flags & gs_hlod_runtime::CutHistory::RESIDENCY_MISSED) {
				residency_missed.set(missed, int32_t(node));
				residency_missed_demanded[missed++] = bool(flags & gs_hlod_runtime::CutHistory::MISS_DEMANDED);
			}
		}
		budget_bound = p_cut.budget_bound || p_cut.first_cap_refused_node >= 0;
		first_cap_refused_node = p_cut.first_cap_refused_node;
		valid = p_cut.status != gs_hlod_runtime::CutHistory::INVALID;
	}
};

#endif
