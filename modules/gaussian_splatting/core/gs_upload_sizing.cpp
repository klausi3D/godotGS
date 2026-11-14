#include "gs_upload_sizing.h"

#include <algorithm>

namespace GSUploadSizing {

uint32_t worker_count(uint32_t p_setting, uint32_t p_logical_cpus,
		uint32_t p_pool_threads, uint32_t p_reserved_pool_threads) {
	uint32_t workers = p_setting;
	if (p_logical_cpus != 0) {
		workers = std::min(workers, std::max(2u, p_logical_cpus / 2u));
	}
	if (p_pool_threads != 0) {
		const uint32_t available = p_pool_threads > p_reserved_pool_threads ?
				p_pool_threads - p_reserved_pool_threads : 0;
		workers = std::min(workers, available);
	}
	return workers;
}

uint64_t staging_clamp_bytes(uint64_t p_staging_max_mb, uint32_t p_frames_in_flight) {
	if (p_staging_max_mb == 0 || p_frames_in_flight == 0) {
		return 0;
	}
	constexpr uint64_t MIB = uint64_t(1024) * 1024;
	// Divide before multiplying without losing the fractional MiB. The remainder
	// is less than a uint32_t frame count, so its MiB product always fits uint64_t.
	const uint64_t whole_mb = p_staging_max_mb / p_frames_in_flight;
	const uint64_t remainder_mb = p_staging_max_mb % p_frames_in_flight;
	if (whole_mb > UINT64_MAX / MIB) {
		return UINT64_MAX;
	}
	const uint64_t whole_bytes = whole_mb * MIB;
	const uint64_t remainder_bytes = remainder_mb * MIB / p_frames_in_flight;
	return remainder_bytes > UINT64_MAX - whole_bytes ? UINT64_MAX : whole_bytes + remainder_bytes;
}

uint64_t frame_limit_bytes(uint64_t p_configured, uint64_t p_staging_clamp,
		bool p_zero_configured_is_unlimited) {
	const uint64_t configured = p_configured == 0 && p_zero_configured_is_unlimited ? UINT64_MAX : p_configured;
	return p_staging_clamp == 0 ? configured : std::min(configured, p_staging_clamp);
}

uint32_t ready_window(uint64_t p_frame_limit_bytes, uint64_t p_unit_bytes,
		uint32_t p_max_jobs_in_flight) {
	if (p_frame_limit_bytes == 0 || p_frame_limit_bytes == UINT64_MAX || p_unit_bytes == 0) {
		return 0;
	}
	const uint64_t whole_units = p_frame_limit_bytes / p_unit_bytes;
	if (whole_units > UINT32_MAX / 2u) {
		return UINT32_MAX;
	}
	const uint64_t remainder = p_frame_limit_bytes % p_unit_bytes;
	// floor(2 * remainder / unit) is 0 or 1. Compare by subtraction so neither
	// doubling the frame limit nor doubling a near-UINT64_MAX remainder can wrap.
	const uint32_t two_frames = uint32_t(whole_units * 2) +
			(remainder >= p_unit_bytes - remainder ? 1u : 0u);
	return std::max(p_max_jobs_in_flight, two_frames);
}

uint32_t dequeue_batch(uint32_t p_worker_threads, uint32_t p_max_jobs_in_flight,
		uint32_t p_max_batch) {
	if (p_max_batch == 0 || p_worker_threads <= 1) {
		return p_max_batch;
	}
	if (p_max_jobs_in_flight == 0) {
		return 1;
	}
	return std::min(p_max_batch, std::max(1u, p_max_jobs_in_flight / p_worker_threads));
}

bool checked_byte_count(uint64_t p_count, uint64_t p_record_stride, uint64_t &r_bytes) {
	if (p_record_stride != 0 && p_count > UINT64_MAX / p_record_stride) {
		r_bytes = 0;
		return false;
	}
	r_bytes = p_count * p_record_stride;
	return true;
}

Slice slice(uint64_t p_node_bytes, uint64_t p_uploaded_so_far,
		uint64_t p_remaining_allowance, uint64_t p_record_stride) {
	if (p_record_stride == 0 || p_uploaded_so_far > p_node_bytes ||
			p_node_bytes % p_record_stride != 0 || p_uploaded_so_far % p_record_stride != 0) {
		return {};
	}
	const uint64_t remaining = p_node_bytes - p_uploaded_so_far;
	Slice result;
	result.valid = true;
	result.bytes = remaining <= p_remaining_allowance ? remaining :
			(p_remaining_allowance / p_record_stride) * p_record_stride;
	result.is_final = result.bytes == remaining;
	return result;
}

} // namespace GSUploadSizing
