#include "gsq_codec.h"
#include "../gaussian_splat_world_paged.h"

// Built with the same zstd include path as the codec (including system-zstd).
// Its working context is transient, but must be admitted BEFORE encode_page.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace gs_gsq {

uint64_t writer_v4_workspace_bytes(const EncodeParams &p_params) {
	const size_t encode = ZSTD_estimateCCtxSize(p_params.zstd_level);
	const size_t decode = ZSTD_estimateDCtxSize();
	if (ZSTD_isError(encode) || ZSTD_isError(decode) || uint64_t(decode) > UINT64_MAX - sizeof(DecodeContext) ||
			uint64_t(encode) > UINT64_MAX - uint64_t(decode) - sizeof(DecodeContext)) {
		return UINT64_MAX;
	}
	// Both contexts can coexist while encoding/validating a page. Raw planes,
	// log scales, encoded page and decoded arrays are charged by WriterV4.
	return uint64_t(encode) + uint64_t(decode) + sizeof(DecodeContext);
}

} // namespace gs_gsq

uint64_t gs_world_paged::gsq_record_stored_bytes_limit(uint32_t p_count, uint32_t p_sh_degree) {
	if (p_count == 0 || p_count > gs_hlod::kMaxNodeSplats || p_sh_degree > gs_gsq::kMaxShDegree) {
		return 0;
	}
	// Maximum planes: 21-bit positions, scale/rotation/opacity/DC, all SH,
	// per-splat normals. parse_page_header checks the tighter actual frame bound.
	const uint32_t sh = p_sh_degree == 0 ? 0 : 9 + 3 * gs_gsq::sh_high_order_for_degree(p_sh_degree);
	const uint32_t planes = 9 + 14 + sh + 6;
	uint64_t bytes = kPayloadRecordHeaderBytes + gs_gsq::kPageHeaderBytes;
	for (uint32_t first = 0; first < p_count; first += gs_gsq::kSubBlockSplats) {
		const uint32_t count = MIN(p_count - first, gs_gsq::kSubBlockSplats);
		bytes += ZSTD_compressBound(size_t(count) * planes);
	}
	return bytes;
}

uint64_t gs_world_paged::gsq_decode_workspace_bytes() {
	const uint32_t sh = 9 + 3 * gs_gsq::sh_high_order_for_degree(gs_gsq::kMaxShDegree);
	// A pooled context may retain its largest prior sub-block. Include capacity
	// growth and SH3 even when this particular read has fewer splats or bands.
	const uint32_t planes = 9 + 14 + sh + 6;
	return ZSTD_estimateDCtxSize() + sizeof(gs_gsq::DecodeContext) +
			uint64_t(2) * gs_gsq::kSubBlockSplats * planes;
}
