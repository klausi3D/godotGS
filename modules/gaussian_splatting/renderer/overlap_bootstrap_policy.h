#pragma once

// Overlap-sort sizing policy (BV-10, ADR beyond-VRAM worlds, section 5.2 and invariant I-6). Pure so
// every boundary is testable without a device.
//
// E3: the overlap sort used to bootstrap at visible * 50, which reaches the 100M hard cap at 2M
// visible splats and costs 2,670 MiB before any record exists. Capacity now follows demand:
// visible * measured records/splat. Before the first measurement a small seed is allocated and the
// bootstrap frame takes one synchronous overlap sample, so a demand above the seed grows in that
// same frame and no record is dropped. max_overlap_records is still the only cap: this sizes the
// allocation, it never limits the records of a frame.
//
// I-6: a replacement is built while the old sorter and buffers are live, and RenderingDevice frees
// them only after the frames in flight, so a resize peaks at old + new.

#include "resident_atlas_budget.h"

#include <cstdint>

namespace GaussianSplatting {

// Seed before any measurement. 8 records/splat is the steady planning figure of the ADR (32M records
// at 4M visible, 855 MiB). It is a seed, not a limit: the bootstrap sample grows past it.
static constexpr uint32_t OVERLAP_BOOTSTRAP_RECORDS_PER_SPLAT = 8u;
// Measured records/splat are kept in 8.8 fixed point: far views measure well below one.
static constexpr uint32_t OVERLAP_RECORDS_PER_SPLAT_ONE = 256u;
// Same +20% as the adaptive budget, so both estimates read the same demand on the same scale.
static constexpr uint32_t OVERLAP_ESTIMATE_HEADROOM_PERCENT = 120u;
// A grow pads to 1.5 * capacity. Every grow recompiles the radix kernels and reallocates hundreds of
// MB, and capacity no longer starts at the hard cap, so a ramp of slowly rising demand must not
// replace the sorter on every step.
static constexpr uint32_t OVERLAP_GROW_FACTOR_PERCENT = 150u;
// RenderingDevice usage does not see the driver. ADR section 5.2 puts driver and others at up to
// 0.6 GiB on the 3080.
static constexpr uint64_t OVERLAP_RESIZE_DRIVER_RESERVE_BYTES = uint64_t(600) * 1024 * 1024;

// 0 means "no sample": an empty view says nothing about the density of the next one.
inline uint32_t overlap_records_per_splat_q8(uint32_t p_raw_records, uint32_t p_visible_splats) {
	if (p_raw_records == 0u || p_visible_splats == 0u) {
		return 0u;
	}
	const uint64_t rate = (uint64_t(p_raw_records) * OVERLAP_RECORDS_PER_SPLAT_ONE + p_visible_splats - 1u) / p_visible_splats;
	return rate > UINT32_MAX ? UINT32_MAX : uint32_t(rate);
}

// Records to reserve at the start of a frame: visible * measured records/splat + headroom, or the
// seed before any measurement. The hard cap is applied by the caller (ensure_resources()).
inline uint32_t overlap_demand_estimate(uint32_t p_visible_splats, uint32_t p_measured_records_per_splat_q8) {
	const uint64_t rate_q8 = p_measured_records_per_splat_q8 > 0u
			? uint64_t(p_measured_records_per_splat_q8) * OVERLAP_ESTIMATE_HEADROOM_PERCENT / 100u + 1u
			: uint64_t(OVERLAP_BOOTSTRAP_RECORDS_PER_SPLAT) * OVERLAP_RECORDS_PER_SPLAT_ONE;
	const uint64_t clamped_rate_q8 = rate_q8 > UINT32_MAX ? uint64_t(UINT32_MAX) : rate_q8;
	const uint64_t records = (uint64_t(p_visible_splats) * clamped_rate_q8 + OVERLAP_RECORDS_PER_SPLAT_ONE - 1u) / OVERLAP_RECORDS_PER_SPLAT_ONE;
	return records > UINT32_MAX ? UINT32_MAX : uint32_t(records);
}

// Per-splat storage must cover the reservation on the indirect route, where the CPU visibility
// readback can lag the GPU. Keep this selection shared with frame-start overlap sizing.
inline uint32_t overlap_effective_visible_splats(uint32_t p_splat_count, uint32_t p_max_visible_splats, bool p_indirect_dispatch) {
	return p_indirect_dispatch && p_max_visible_splats > p_splat_count ? p_max_visible_splats : p_splat_count;
}

struct OverlapOverflowRecoveryPlan {
	bool handle_overflow = false;
	uint32_t requested_records = 0u;
};

inline OverlapOverflowRecoveryPlan overlap_overflow_recovery_plan(bool p_hlod_relative, bool p_first_frame_complete,
		bool p_overflow_detected, uint32_t p_last_unclamped_total) {
	OverlapOverflowRecoveryPlan plan;
	plan.handle_overflow = !p_hlod_relative && p_first_frame_complete && p_overflow_detected;
	if (plan.handle_overflow) {
		const uint64_t needed = uint64_t(p_last_unclamped_total) * 3u / 2u;
		plan.requested_records = needed > UINT32_MAX ? UINT32_MAX : uint32_t(needed);
	}
	return plan;
}

struct OverlapFrameSizingInput {
	uint32_t splat_count = 0u;
	uint32_t effective_visible_splats = 0u;
	uint32_t current_cpu_draw_domain_count = 0u;
	// Raw demand and the CPU draw domain from the same completed COUNT/readback request.
	uint32_t sampled_records = 0u;
	uint32_t sampled_cpu_draw_domain_count = 0u;
	uint32_t total_gaussians = 0u;
	// Actual capacity AFTER any overflow recovery attempt, including a refused/failed resize.
	uint32_t capacity = 0u;
	uint32_t measured_records_per_splat_q8 = 0u;
	uint32_t adaptive_overlap_floor = 0u;
	bool adaptive_has_history = false;
};

struct OverlapFrameSizingPlan {
	uint32_t overlap_base_count = 0u;
	bool bootstrap_floor_applied = false;
	uint32_t bootstrap_estimate = 0u;
	uint32_t requested_records = 0u;
	// Capacity needed if the request is served, retaining a larger warmed allocation. The resource
	// owner still applies caps, optional growth padding, shrink hysteresis and resize preflight.
	uint32_t capacity = 0u;
};

// Pure caller policy. Allocation and its failure paths remain with the resource owner.
inline OverlapFrameSizingPlan overlap_frame_sizing_plan(const OverlapFrameSizingInput &p_input) {
	OverlapFrameSizingPlan plan;
	plan.overlap_base_count = p_input.splat_count > p_input.effective_visible_splats ? p_input.splat_count : p_input.effective_visible_splats;
	uint32_t estimate = overlap_demand_estimate(plan.overlap_base_count, p_input.measured_records_per_splat_q8);
	plan.bootstrap_floor_applied = p_input.capacity == 0u && p_input.total_gaussians > 0u && plan.overlap_base_count < 1024u;
	if (plan.bootstrap_floor_applied) {
		// Streaming warm-up can expose a tiny visible set while the atlas holds a larger world.
		const uint32_t bootstrap_cap = 8000000u;
		const uint32_t bootstrap_floor = p_input.total_gaussians < bootstrap_cap ? p_input.total_gaussians : bootstrap_cap;
		estimate = estimate > bootstrap_floor ? estimate : bootstrap_floor;
	}
	plan.bootstrap_estimate = estimate;
	if (p_input.adaptive_overlap_floor > 0u) {
		// With history but no measured rate, use the adaptive budget alone. Otherwise the demand
		// estimate can lead the budget, which only sees a rise in splats after readback completes.
		if (p_input.adaptive_has_history && p_input.measured_records_per_splat_q8 == 0u) {
			estimate = p_input.adaptive_overlap_floor;
		} else {
			estimate = estimate > p_input.adaptive_overlap_floor ? estimate : p_input.adaptive_overlap_floor;
		}
	}
	if (p_input.sampled_records > 0u && p_input.sampled_cpu_draw_domain_count > 0u &&
			p_input.current_cpu_draw_domain_count > p_input.sampled_cpu_draw_domain_count) {
		// Predict only domain growth, using the existing Q8 rate and headroom convention.
		// Keeping today's estimate as a floor leaves unchanged/shrinking domains on their
		// existing sizing and shrink policy. The resource owner still enforces the hard cap.
		const uint32_t sampled_rate_q8 = overlap_records_per_splat_q8(p_input.sampled_records, p_input.sampled_cpu_draw_domain_count);
		const uint32_t predicted = overlap_demand_estimate(p_input.current_cpu_draw_domain_count, sampled_rate_q8);
		estimate = estimate > predicted ? estimate : predicted;
	}
	plan.requested_records = estimate;
	plan.capacity = p_input.capacity > estimate ? p_input.capacity : estimate;
	return plan;
}

// Capacity a grow should aim for. Never below the request, and the padding never crosses the hard
// cap (0 = no cap).
inline uint32_t overlap_grow_target(uint32_t p_requested_records, uint32_t p_capacity, uint32_t p_hard_cap) {
	const uint64_t padded = uint64_t(p_capacity) * OVERLAP_GROW_FACTOR_PERCENT / 100u;
	uint64_t target = padded > p_requested_records ? padded : uint64_t(p_requested_records);
	if (p_hard_cap > 0u && target > p_hard_cap) {
		target = p_hard_cap > p_requested_records ? p_hard_cap : p_requested_records;
	}
	return target > UINT32_MAX ? UINT32_MAX : uint32_t(target);
}

enum class OverlapResizePreflight : uint8_t {
	FITS = 0,
	REFUSED = 1,
	UNVERIFIED = 2, // No usable capacity or usage figure: the resize proceeds, and nothing was proven.
};

// Pre-flight of a live replacement. p_device_used_bytes is what is allocated now, so it already holds
// the old sorter and buffers; p_new_bytes is the replacement's full size. Refused when old + new does
// not fit, even if new alone would. Until the device ledger (BV-11) owns this, the bound is the device
// heap minus the driver reserve, not the 0.60 GS share, which ADR D-11 keeps report-only on this route.
inline OverlapResizePreflight overlap_resize_preflight(uint64_t p_device_used_bytes, uint64_t p_new_bytes,
		uint64_t p_device_capacity_bytes) {
	if (p_device_capacity_bytes < ResidentAtlasBudget::kMinTrustedDeviceBudgetBytes || p_device_used_bytes == 0u) {
		return OverlapResizePreflight::UNVERIFIED;
	}
	const uint64_t limit = p_device_capacity_bytes > OVERLAP_RESIZE_DRIVER_RESERVE_BYTES
			? p_device_capacity_bytes - OVERLAP_RESIZE_DRIVER_RESERVE_BYTES
			: 0u;
	if (p_device_used_bytes > limit || p_new_bytes > limit - p_device_used_bytes) {
		return OverlapResizePreflight::REFUSED;
	}
	return OverlapResizePreflight::FITS;
}

} // namespace GaussianSplatting
