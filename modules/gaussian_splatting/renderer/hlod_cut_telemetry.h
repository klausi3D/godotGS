#ifndef GS_HLOD_CUT_TELEMETRY_H
#define GS_HLOD_CUT_TELEMETRY_H

#include "../lod/hlod_view_set.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

#include <cstring>

// Render-thread-owned, report-only snapshot of the same selection used for the world draw list.
// The selector's diagnostic scratch is preallocated. Packed exports use COW when readers retain stats.
struct HlodCutTelemetry {
	bool valid = false;
	PackedInt32Array drawn_ids;
	PackedFloat32Array drawn_fades; // Parallel to drawn_ids (sorted by id): the record's S6 fade (ADR 6.7).
	uint32_t fade_pairs = 0;
	uint32_t fade_refused = 0;
	uint32_t fade_groups = 0;
	uint32_t fade_snapped = 0;
	uint64_t fade_snapped_total = 0; // Cumulative over captures, report only.
	uint64_t fade_extra_splats = 0;
	String drawn_digest;
	PackedInt32Array hysteresis_held;
	PackedInt32Array residency_missed;
	Array residency_missed_demanded;
	bool budget_bound = false;
	int32_t first_cap_refused_node = -1;
	// Render-thread-owned scalar observations; capture and export use the existing stats boundary.
	gs_hlod_runtime::CutHistory::Result page_report;
	uint64_t page_refused_groups_total = 0; // Cumulative diagnostic captures, never a policy input.

	void append_stats(Dictionary &r_stats, bool p_drawn_valid) const {
		const bool drawn_valid = valid && p_drawn_valid;
		r_stats["hlod_drawn_node_ids"] = drawn_valid ? Variant(drawn_ids) : Variant();
		r_stats["hlod_drawn_node_digest"] = drawn_valid ? Variant(drawn_digest) : Variant();
		r_stats["hlod_drawn_node_fades"] = drawn_valid ? Variant(drawn_fades) : Variant();
		r_stats["hlod_fade_pairs"] = valid ? Variant(int64_t(fade_pairs)) : Variant();
		r_stats["hlod_fade_refused"] = valid ? Variant(int64_t(fade_refused)) : Variant();
		r_stats["hlod_fade_groups"] = valid ? Variant(int64_t(fade_groups)) : Variant();
		r_stats["hlod_fade_snapped"] = valid ? Variant(int64_t(fade_snapped)) : Variant();
		r_stats["hlod_fade_snapped_total"] = Variant(int64_t(fade_snapped_total));
		r_stats["hlod_fade_extra_splats"] = valid ? Variant(int64_t(fade_extra_splats)) : Variant();
		r_stats["hlod_cut_hysteresis_held"] = valid ? Variant(hysteresis_held) : Variant();
		r_stats["hlod_cut_budget_bound"] = valid ? Variant(budget_bound) : Variant();
		r_stats["hlod_cut_first_cap_refused_node"] = valid ? Variant(first_cap_refused_node) : Variant();
		r_stats["hlod_cut_residency_missed"] = valid ? Variant(residency_missed) : Variant();
		// Array has reference semantics: a retained stats snapshot must survive the next capture.
		r_stats["hlod_cut_residency_missed_demanded"] = valid ? Variant(residency_missed_demanded.duplicate()) : Variant();
		const bool pages_measured = valid && page_report.closure_measured;
		r_stats["hlod_stream_closure_measured"] = pages_measured;
		r_stats["hlod_stream_demanded_closure_pages"] = pages_measured ? Variant(int64_t(page_report.demanded_closure_pages)) : Variant();
		r_stats["hlod_stream_page_budget"] = pages_measured ? Variant(int64_t(page_report.page_budget)) : Variant();
		r_stats["hlod_stream_bootstrap_bypass_pages"] = pages_measured ? Variant(int64_t(page_report.bootstrap_bypass_pages)) : Variant();
		r_stats["hlod_stream_resident_refinement_bypass_pages"] = pages_measured ? Variant(int64_t(page_report.resident_refinement_bypass_pages)) : Variant();
		r_stats["hlod_stream_bootstrap_bypass_over_budget"] = pages_measured ? Variant(page_report.bootstrap_bypass_over_budget) : Variant();
		r_stats["hlod_stream_resident_refinement_bypass_over_budget"] = pages_measured ? Variant(page_report.resident_refinement_bypass_over_budget) : Variant();
		r_stats["hlod_stream_page_refused_groups"] = pages_measured ? Variant(int64_t(page_report.page_refused_groups)) : Variant();
		r_stats["hlod_stream_page_refused_groups_total"] = pages_measured ? Variant(int64_t(page_refused_groups_total)) : Variant();
	}

	void capture(const gs_hlod_runtime::CutHistory &p_history, const gs_hlod_runtime::CutHistory::Result &p_cut,
			uint32_t p_nodes, const gs_hlod_runtime::DrawRecord *p_records, uint32_t p_count) {
		// Report-only: sort ids with their record index so each fade stays with its id (a node is drawn once per cut).
		LocalVector<uint64_t> order;
		order.resize(p_count);
		for (uint32_t i = 0; i < p_count; ++i) { order[i] = (uint64_t(p_records[i].node) << 32) | i; }
		order.sort();
		drawn_ids.resize(p_count);
		drawn_fades.resize(p_count);
		for (uint32_t i = 0; i < p_count; ++i) {
			drawn_ids.set(i, int32_t(order[i] >> 32));
			drawn_fades.set(i, p_records[uint32_t(order[i])].fade);
		}
		fade_pairs = p_cut.fade_pairs;
		fade_refused = p_cut.fade_refused;
		fade_groups = p_cut.fade_groups;
		fade_snapped = p_cut.fade_snapped;
		fade_snapped_total += p_cut.fade_snapped;
		fade_extra_splats = p_cut.fade_extra_splats;
		// FNV-1a over sorted uint32 IDs encoded little-endian, independent of host byte order. A record that is blending
		// (S6/S6b fade != 1) also hashes its fade's bits after its ID: the digest names what is drawn, so it keeps
		// changing while a fade is in flight and a "stable digest" settle waits for the blend to finish. With no blend
		// every fade is 1 and the digest is exactly the ID-only digest.
		uint64_t digest = UINT64_C(14695981039346656037);
		auto mix_u32 = [&digest](uint32_t p_value) {
			for (uint32_t shift = 0; shift < 32; shift += 8) {
				digest ^= (p_value >> shift) & 0xffu;
				digest *= UINT64_C(1099511628211);
			}
		};
		for (int i = 0; i < drawn_ids.size(); ++i) {
			mix_u32(uint32_t(drawn_ids[i]));
			const float f = drawn_fades[i];
			if (f != 1.0f) {
				uint32_t bits = 0;
				memcpy(&bits, &f, sizeof(bits));
				mix_u32(0xFADEFADEu);
				mix_u32(bits);
			}
		}
		drawn_digest = String::num_uint64(digest, 16).lpad(16, "0"); // pad_zeros() would pad after a leading hex letter.
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
		page_report = p_cut;
		if (p_cut.status != gs_hlod_runtime::CutHistory::INVALID && p_cut.closure_measured) {
			page_refused_groups_total += p_cut.page_refused_groups;
		}
		valid = p_cut.status != gs_hlod_runtime::CutHistory::INVALID;
	}
};

#endif
