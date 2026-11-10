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
