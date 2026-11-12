#pragma once

#include "../core/gaussian_splat_hlod_tiled_bake.h"

// D2-3 phase 1: declarations and unavailable stubs only.
// Phase 2: plain v3 + bit 7 GSQ input only; validate header/directory at open,
// then reuse the GSQ decoder with stored AND decoded digest verification always
// enabled. No public switch may disable either check. Descriptors partition
// [0, splat_count); each read owns its buffers in ascending global input order.
// After open, concurrent descriptor/read/stat calls must be safe. Decoder state
// must be per-call or leased; shared counters/reasons must use one documented lock.
class GsqAutoTreeSource : public gs_hlod::TiledBakeSource {
public:
	struct Stats {
		uint64_t page_count = 0;
		uint64_t pages_decoded = 0;
		uint64_t pages_decoded_digest_checked = 0;
		uint64_t failures = 0;
		String last_reason;
	};

	Error open(const String &p_path, String *r_error = nullptr);
	gs_hlod::TiledBakeSourceInfo get_info() const override;
	Error get_chunk_info(uint32_t p_chunk, gs_hlod::TiledBakeChunkInfo &r_info, String *r_error = nullptr) const override;
	Error read_chunk(uint32_t p_chunk, gs_hlod::TiledBakeChunk &r_chunk, String *r_error = nullptr) const override;
	Stats get_stats() const;
};
