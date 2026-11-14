#ifndef GS_SPLAT_COUNT_SAMPLE_H
#define GS_SPLAT_COUNT_SAMPLE_H

#include <cstdint>

namespace gs_count_provenance {
// CPU bookkeeping only. Owned by the sorting pipeline and accessed in the same
// serialized RenderingDevice callback/render context as its existing count cache.
struct Sample {
	uint32_t count = 0;
	uint32_t frame = 0;
	bool valid = false;
	// The renderer's synchronous zero path overwrites the cached value while
	// retaining its legacy bootstrap/rejection state. That value is unmeasured.
	bool source_valid = true;

	int64_t source_frame() const { return valid && source_valid ? int64_t(frame) : -1; }
	bool accept(uint32_t p_count, uint32_t p_request_frame) {
		if (valid && p_request_frame < frame) {
			return false;
		}
		count = p_count;
		frame = p_request_frame;
		valid = true;
		source_valid = true;
		return true;
	}
};
} // namespace gs_count_provenance

#endif // GS_SPLAT_COUNT_SAMPLE_H
