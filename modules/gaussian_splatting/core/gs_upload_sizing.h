#ifndef GS_UPLOAD_SIZING_H
#define GS_UPLOAD_SIZING_H

#include <cstdint>

// Caller-supplied policy only: no settings, allocations, or route defaults.
namespace GSUploadSizing {

// 0 setting disables workers. Unknown (0) CPU/pool inputs impose no clamp.
// Known CPUs clamp to max(2, cpus / 2); known pool capacity clamps to
// max(0, pool_threads - reserved_pool_threads). Reservations never underflow.
uint32_t worker_count(uint32_t p_setting, uint32_t p_logical_cpus,
		uint32_t p_pool_threads, uint32_t p_reserved_pool_threads);

// 0 MiB or frames means unknown (0). Otherwise floor(MiB * 2^20 / frames),
// with the exact result saturated at UINT64_MAX, never a wrapped product.
uint64_t staging_clamp_bytes(uint64_t p_staging_max_mb, uint32_t p_frames_in_flight);

// A 0 clamp means unknown. The caller explicitly selects whether configured
// 0 means unlimited (chunk) or 0 (a caller may reject this configuration).
uint64_t frame_limit_bytes(uint64_t p_configured, uint64_t p_staging_clamp,
		bool p_zero_configured_is_unlimited);

// floor(2 * frame_limit / unit_bytes), saturated to UINT32_MAX and at least
// the supplied jobs ceiling. 0/UINT64_MAX limits or a 0 unit return 0 (unbounded
// or invalid). The unit is a caller input; this helper knows no chunk size.
uint32_t ready_window(uint64_t p_frame_limit_bytes, uint64_t p_unit_bytes,
		uint32_t p_max_jobs_in_flight);

// One worker can dequeue max_batch; other workers share a bounded jobs window.
// A 0 jobs window gives each worker one item. max_batch is caller policy.
uint32_t dequeue_batch(uint32_t p_worker_threads, uint32_t p_max_jobs_in_flight,
		uint32_t p_max_batch);

// Reports overflow with false and out=0; a zero count or stride is a valid
// zero-byte product. Slice validation separately rejects a zero stride.
bool checked_byte_count(uint64_t p_count, uint64_t p_record_stride, uint64_t &r_bytes);

struct Slice {
	bool valid = false;
	uint64_t bytes = 0;
	bool is_final = false;
};

// Node bytes and uploaded bytes must represent whole records, with uploaded
// <= node and stride > 0. Invalid inputs return {false, 0, false}. Empty/already
// complete nodes return {true, 0, true}; sub-stride allowance makes no progress
// and returns {true, 0, false}. Other slices are stride-aligned, bounded by the
// allowance, and final iff they equal the exact remaining node bytes.
Slice slice(uint64_t p_node_bytes, uint64_t p_uploaded_so_far,
		uint64_t p_remaining_allowance, uint64_t p_record_stride);

} // namespace GSUploadSizing

#endif // GS_UPLOAD_SIZING_H
