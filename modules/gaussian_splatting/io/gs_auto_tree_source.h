#pragma once

#include "../core/gaussian_splat_hlod_tiled_bake.h"
#include "gsq/gsq_codec.h"

#include "core/os/mutex.h"

// D2-3: plain v3 + bit 7 GSQ input only; validate header/directory at open,
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

	// Configure before publication; open must not race any other method. All
	// successful-open state is immutable thereafter except stats, guarded below.
	Error open(const String &p_path, String *r_error = nullptr, uint64_t p_max_ram_bytes = UINT64_MAX);
	gs_hlod::TiledBakeSourceInfo get_info() const override;
	Error get_chunk_info(uint32_t p_chunk, gs_hlod::TiledBakeChunkInfo &r_info, String *r_error = nullptr) const override;
	Error read_chunk(uint32_t p_chunk, gs_hlod::TiledBakeChunk &r_chunk, String *r_error = nullptr) const override;
	Stats get_stats() const;
	uint32_t get_sh_degree() const { return sh_degree; }
	bool is_2d() const { return mode_2d; }
	AABB get_bounds() const { return bounds; }
	// Conservative retained directory/path estimate, excluding caller-owned output.
	uint64_t get_reserved_bytes() const { return reserved_bytes; }
	// Admitted transient loader/index/directory reservation during open().
	uint64_t get_open_peak_reserved_bytes() const { return open_peak_reserved_bytes; }
	// Per concurrent read: stored-page buffer plus decoder/zstd workspace allowance.
	uint64_t get_decode_scratch_bytes() const { return decode_scratch_bytes; }

private:
	struct Page {
		uint64_t offset = 0;
		uint32_t stored_bytes = 0;
		uint8_t stored_digest[gs_gsq::kDigestBytes] = {};
		uint8_t decoded_digest[gs_gsq::kDigestBytes] = {};
		uint32_t extent[6] = {};
		gs_hlod::TiledBakeChunkInfo chunk;
	};
	String file_path;
	uint64_t file_bytes = 0;
	gs_hlod::TiledBakeSourceInfo info;
	LocalVector<Page> pages;
	gs_gsq::EncodeParams codec_params;
	uint32_t sh_degree = 0;
	bool mode_2d = false;
	AABB bounds;
	uint64_t reserved_bytes = 0;
	uint64_t open_peak_reserved_bytes = 0;
	uint64_t decode_scratch_bytes = 0;
	// The sole lock for every shared counter and reason; decoding and FileAccess
	// are per-call, and immutable directory state needs no lock after open.
	mutable BinaryMutex stats_mutex;
	mutable Stats stats;
	Error fail(Error p_error, const String &p_reason, String *r_error) const;
};
