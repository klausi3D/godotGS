#include "gs_upload_sizing.h"

namespace GSUploadSizing {

uint32_t worker_count(uint32_t, uint32_t, uint32_t, uint32_t) {
	// D3-4 phase-1 stub: boundary tests must fail until policy is implemented.
	return 0;
}

uint64_t staging_clamp_bytes(uint64_t, uint32_t) {
	// D3-4 phase-1 stub.
	return 0;
}

uint64_t frame_limit_bytes(uint64_t, uint64_t, bool) {
	// D3-4 phase-1 stub.
	return 0;
}

uint32_t ready_window(uint64_t, uint64_t, uint32_t) {
	// D3-4 phase-1 stub.
	return 0;
}

uint32_t dequeue_batch(uint32_t, uint32_t, uint32_t) {
	// D3-4 phase-1 stub.
	return 0;
}

bool checked_byte_count(uint64_t, uint64_t, uint64_t &r_bytes) {
	// D3-4 phase-1 stub.
	r_bytes = 0;
	return false;
}

Slice slice(uint64_t, uint64_t, uint64_t, uint64_t) {
	// D3-4 phase-1 stub.
	return {};
}

} // namespace GSUploadSizing
