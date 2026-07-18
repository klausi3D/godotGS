#ifndef GPU_SORTING_CONSTANTS_H
#define GPU_SORTING_CONSTANTS_H

#include <cstdint>

namespace GPUSortingConstants {

static constexpr uint32_t DEFAULT_WORKGROUP_SIZE = 256;
// Radix-sort digit width PER PASS (4-bit = 16 passes for a 64-bit key, 8-bit = 8 passes). This is a
// performance/mechanics knob ONLY — it does NOT change the sorted output or key precision (precision is
// key_bits/tile_bits/depth_bits). Both 4 and 8 are valid (validate() accepts both) and both are now
// CORRECT at any workgroup_size (the strided per-bin shader loops in gpu_sorter.cpp + the shared-memory
// probe in RadixSort::is_supported handle 8-bit's 256 bins). DEFAULT IS 4-BIT: measured A/B on
// GrandmasHouse (2026-06-06) showed 8-bit is SLOWER, not faster — fewer passes are outweighed by 16x
// larger histograms, more per-pass bin work, and lower occupancy (~10 KB shared mem). Keep 4-bit unless
// a future measurement on different hardware/workload shows otherwise.
static constexpr uint32_t DEFAULT_RADIX_BITS = 4;
static constexpr uint32_t RADIX_BITS = 8;
static constexpr uint32_t RADIX_SIZE = 1u << RADIX_BITS;
static constexpr uint32_t MAX_WORKGROUP_SIZE = 1024;
static constexpr uint32_t HISTOGRAM_BINS = RADIX_SIZE;

// Sort-key bit layout defaults. 64-bit (32 tile + 32 depth) is the ONLY
// shippable layout: 32-bit quantized depth keys flicker/band on real-scan data
// (GS-298). These are the single source of truth for the key-width defaults so
// the ProjectSettings GLOBAL_DEF and every get_setting() fallback agree — a read
// before registration can then never silently select the broken 32-bit path.
// (32-bit remains reachable only via an explicit gpu_preset="custom" opt-in.)
static constexpr uint32_t DEFAULT_KEY_BITS = 64;
static constexpr uint32_t DEFAULT_TILE_BITS = 32;
static constexpr uint32_t DEFAULT_DEPTH_BITS = 32;

// ---------------------------------------------------------------------------
// Sort-path allocation bound
// ---------------------------------------------------------------------------
// RenderingDevice::storage_buffer_create() takes the size as a **uint32_t**
// (servers/rendering/rendering_device.h). Every buffer the sort path allocates is
// sized from the element count, so a large enough element count produces a size
// that silently TRUNCATES modulo 2^32: the device hands back a buffer far smaller
// than the shaders index, and the sort writes out of bounds. That is silent VRAM
// corruption, which is strictly worse than a clean rejection — so the element
// count must be bounded by the LARGEST buffer the path allocates, not just by the
// key buffer.
//
// The dominant term is NOT the key buffer. RadixSort preallocates a per-workgroup,
// per-bin, per-pass histogram (and an identically sized workgroup-prefix buffer):
//
//   workgroups      = ceil(N / workgroup_size)
//   radix_size      = 1 << radix_bits
//   num_passes      = ceil(key_bits / radix_bits)
//   histogram_bytes = workgroups * radix_size * num_passes * 4
//
// which scales as N * (radix_size * num_passes * 4 / workgroup_size). At the
// permissive end of the validated knob ranges (workgroup_size=64, radix_bits=8,
// key_bits=64) that is 128 bytes PER ELEMENT — 16x the 8-byte 64-bit key buffer.
// The safe element count therefore depends on radix_bits / workgroup_size /
// key_bits and cannot be expressed as one scalar constant.
//
// Sentinel returned by sort_path_max_buffer_bytes() when the configuration is not
// one the sort path can build at all (an unsupported radix_bits). It is
// deliberately UINT64_MAX so that every "does this fit in RenderingDevice's
// uint32_t size parameter?" comparison FAILS CLOSED: an unsupported config can
// never be mistaken for a small, allocatable one. Callers that want to *report*
// the condition (rather than just reject it) must compare against this sentinel
// before formatting a byte count — see GPUSortingConfig::get_validation_errors().
static constexpr uint64_t SORT_PATH_SIZE_UNSUPPORTED = UINT64_MAX;

// The radix widths the sort path can actually build. RadixSort::create_variant()
// and GPUSortingConfig::validate() accept exactly this set. Anything else has no
// defined buffer sizing, and evaluating `1ull << radix_bits` for it would be
// UNDEFINED BEHAVIOUR once radix_bits >= 64 (a shift count at or above the width
// of the promoted type). Every shift by a radix width must go through this check
// first, so the helper is total for ANY input instead of safe only when the
// caller happens to pre-validate.
inline bool is_supported_radix_bits(uint32_t p_radix_bits) {
	return p_radix_bits == 4 || p_radix_bits == 8;
}

// Returns the largest single storage_buffer_create() size, in bytes, computed in
// 64-bit so the result is the TRUE required size even when it exceeds uint32.
// Callers reject when the result exceeds UINT32_MAX. Bounding the byte total also
// bounds RadixSort's uint32 `histogram_stride` intermediate (stride <= bytes / 4).
//
// TOTAL: defined for every possible argument. An unsupported radix_bits returns
// SORT_PATH_SIZE_UNSUPPORTED instead of shifting by it. The radix check is FIRST
// — before the zero-element short-circuit — so an unsupported configuration is
// reported as unsupported no matter what element count accompanies it.
inline uint64_t sort_path_max_buffer_bytes(uint64_t p_max_elements, uint32_t p_radix_bits,
		uint32_t p_workgroup_size, uint32_t p_key_bits) {
	if (!is_supported_radix_bits(p_radix_bits)) {
		return SORT_PATH_SIZE_UNSUPPORTED;
	}
	if (p_max_elements == 0) {
		return 0;
	}
	const uint32_t radix_bits = p_radix_bits;
	// workgroup_size only ever appears as a DIVISOR here, so it cannot invoke UB
	// the way a shift count can; it is still coerced away from zero to keep the
	// division defined for any input.
	const uint32_t workgroup_size = p_workgroup_size > 0 ? p_workgroup_size : DEFAULT_WORKGROUP_SIZE;
	const uint32_t key_bits = p_key_bits > 32 ? 64u : 32u;

	// --- RadixSort (gpu_sorter.cpp RadixSort::initialize) ---
	const uint64_t radix_size = 1ull << radix_bits; // radix_bits is 4 or 8 here — never >= 64.
	uint64_t num_passes = (uint64_t(key_bits) + radix_bits - 1ull) / radix_bits;
	if (num_passes == 0) {
		num_passes = 1;
	}
	uint64_t workgroups = (p_max_elements + workgroup_size - 1ull) / workgroup_size;
	if (workgroups == 0) {
		workgroups = 1;
	}
	// histogram_buffer and wg_prefix_buffer are both this size.
	const uint64_t histogram_bytes = workgroups * radix_size * num_passes * sizeof(uint32_t);
	const uint64_t key_stride_bytes = (key_bits > 32) ? 8ull : 4ull;
	const uint64_t temp_keys_bytes = p_max_elements * key_stride_bytes;
	const uint64_t temp_values_bytes = p_max_elements * sizeof(uint32_t);

	// --- OneSweepSort (fixed WORKGROUP_SIZE/RADIX_SIZE, 32-bit keys) ---
	const uint64_t onesweep_workgroups =
			(p_max_elements + DEFAULT_WORKGROUP_SIZE - 1ull) / DEFAULT_WORKGROUP_SIZE;
	const uint64_t onesweep_histogram_bytes = onesweep_workgroups * RADIX_SIZE * sizeof(uint32_t);

	uint64_t largest = histogram_bytes;
	if (temp_keys_bytes > largest) {
		largest = temp_keys_bytes;
	}
	if (temp_values_bytes > largest) {
		largest = temp_values_bytes;
	}
	if (onesweep_histogram_bytes > largest) {
		largest = onesweep_histogram_bytes;
	}
	return largest;
}

// True when every buffer the sort path allocates for this configuration fits in
// RenderingDevice's uint32_t size parameter (i.e. nothing truncates). An
// unsupported radix_bits yields SORT_PATH_SIZE_UNSUPPORTED (= UINT64_MAX), which
// is > UINT32_MAX, so this correctly returns false for it — fail closed.
inline bool sort_path_allocation_fits_device_size(uint64_t p_max_elements, uint32_t p_radix_bits,
		uint32_t p_workgroup_size, uint32_t p_key_bits) {
	return sort_path_max_buffer_bytes(p_max_elements, p_radix_bits, p_workgroup_size, p_key_bits) <=
			uint64_t(UINT32_MAX);
}

} // namespace GPUSortingConstants

#endif // GPU_SORTING_CONSTANTS_H
