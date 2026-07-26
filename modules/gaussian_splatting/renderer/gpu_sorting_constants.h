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

// ---------------------------------------------------------------------------
// RadixSort scatter-kernel shared-memory bound (#634)
// ---------------------------------------------------------------------------
// The RadixSort scatter kernel is the shared-memory PEAK of the sort path. It
// allocates, in compute shared memory:
//
//   local_bases[radix_size] + local_offsets[radix_size] + bin_masks[radix_size * mask_words]
//
// where radix_size = 1 << radix_bits and mask_words = ceil(workgroup_size / 32)
// (one uint bitmask word per 32 workgroup lanes). radix_bits and workgroup_size
// are each validated INDEPENDENTLY, but the constraint is on their PRODUCT: e.g.
// (radix_bits=8, workgroup_size=512) needs 256*(2+16)*4 = 18432 bytes, which
// exceeds the shared-memory budget of common Intel/AMD parts. When the scatter
// pipeline cannot be built the renderer falls through to compositing translucent
// splats UNSORTED — mathematically wrong output. See #634 / #586.
//
// This helper is the SINGLE SOURCE OF TRUTH for that derived quantity:
//   * RadixSort::is_supported() (gpu_sorter.cpp) probes the ACTUAL device limit
//     against this value, and
//   * GPUSortingConfig::get_validation_errors() flags any pair whose requirement
//     exceeds VULKAN_MIN_COMPUTE_SHARED_MEMORY_BYTES at configuration time.
// Neither re-derives the formula by hand.

// Sentinel returned for a radix width the scatter kernel cannot build. Same
// fail-closed rationale as SORT_PATH_SIZE_UNSUPPORTED: it is UINT32_MAX so every
// "does this fit in <limit>?" comparison FAILS CLOSED — an unsupported radix can
// never be mistaken for a small, buildable footprint. `1u << radix_bits` is UB
// once radix_bits >= 32, so the helper returns this instead of shifting.
static constexpr uint32_t RADIX_SCATTER_SHARED_MEMORY_UNSUPPORTED = UINT32_MAX;

// Vulkan 1.1 guarantees maxComputeSharedMemorySize >= 16384 bytes (16 KB) on every
// conformant device (docs/PLATFORM_COMPATIBILITY.md requires Vulkan 1.1+); common
// Intel/AMD parts sit exactly at this floor. A (radix_bits, workgroup_size) pair
// whose scatter footprint exceeds this PORTABLE minimum is guaranteed to fail
// RadixSort::is_supported() on some supported hardware, so it can be rejected at
// CONFIG time — before any device exists — instead of surfacing as silently
// unsorted output many frames later.
static constexpr uint32_t VULKAN_MIN_COMPUTE_SHARED_MEMORY_BYTES = 16384u;

// Bytes of compute shared memory the RadixSort scatter kernel requires for a given
// (radix_bits, workgroup_size). TOTAL: an unsupported radix_bits returns
// RADIX_SCATTER_SHARED_MEMORY_UNSUPPORTED instead of shifting by it. A zero
// workgroup_size is coerced to DEFAULT_WORKGROUP_SIZE (matching the runtime probe
// and sort_path_max_buffer_bytes) so the result is defined for any input.
inline uint32_t radix_scatter_shared_memory_bytes(uint32_t p_radix_bits, uint32_t p_workgroup_size) {
	if (!is_supported_radix_bits(p_radix_bits)) {
		return RADIX_SCATTER_SHARED_MEMORY_UNSUPPORTED;
	}
	const uint32_t workgroup_size = p_workgroup_size > 0 ? p_workgroup_size : DEFAULT_WORKGROUP_SIZE;
	const uint32_t radix_size = 1u << p_radix_bits; // radix_bits is 4 or 8 here — never >= 32.
	const uint32_t mask_words = (workgroup_size + 31u) / 32u;
	const uint32_t scatter_shared_uints = radix_size * (2u + mask_words);
	return scatter_shared_uints * uint32_t(sizeof(uint32_t));
}

} // namespace GPUSortingConstants

#endif // GPU_SORTING_CONSTANTS_H
