#include "gsq_codec.h"

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
