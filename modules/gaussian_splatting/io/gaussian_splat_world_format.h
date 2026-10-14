#ifndef GAUSSIAN_SPLAT_WORLD_FORMAT_H
#define GAUSSIAN_SPLAT_WORLD_FORMAT_H

#include "core/error/error_list.h"

#include <cstdint>

// Header selection shared by the runtime loader and editor importer. This is
// explicit format identity; payload contents never select a different schema.
namespace gs_world_format {

static constexpr uint32_t kVersionV1 = 1u;
static constexpr uint32_t kVersionV2 = 2u;
static constexpr uint32_t kVersion = 3u;
static constexpr uint32_t kFlagHasHlod = 1u << 6u;
static constexpr uint32_t kPlainFlags = (1u << 6u) - 1u;
static constexpr uint64_t kLegacyPrefixBytes = 104u;
static constexpr uint64_t kPrefixBytes = 112u;
static constexpr uint64_t kHlodExtensionBytes = 80u;

struct HeaderLayout {
	uint64_t prefix_bytes = 0u;
	uint64_t header_bytes = 0u;
	bool has_hlod = false;
	bool has_layout_word = false;
};

inline Error select_header(uint32_t p_version, uint32_t p_flags, uint64_t p_file_bytes, HeaderLayout &r_layout) {
	HeaderLayout layout;
	if (p_version != kVersionV1 && p_version != kVersionV2 && p_version != kVersion) {
		return ERR_FILE_CORRUPT;
	}
	layout.has_hlod = (p_flags & kFlagHasHlod) != 0u;
	if ((p_flags & ~(kPlainFlags | kFlagHasHlod)) != 0u) {
		return layout.has_hlod ? ERR_FILE_CORRUPT : ERR_FILE_UNRECOGNIZED;
	}
	if (p_version == kVersionV1 && layout.has_hlod) {
		return ERR_FILE_CORRUPT;
	}
	if (p_version == kVersionV2 && !layout.has_hlod) {
		// The 112-byte plain-v2 prototype collided with the released HLOD-v2
		// identity. It requires re-export; never infer it from its payload.
		return ERR_FILE_UNRECOGNIZED;
	}
	layout.has_layout_word = p_version == kVersion;
	layout.prefix_bytes = layout.has_layout_word ? kPrefixBytes : kLegacyPrefixBytes;
	layout.header_bytes = layout.prefix_bytes + (layout.has_hlod ? kHlodExtensionBytes : 0u);
	if (p_file_bytes < layout.header_bytes) {
		return ERR_FILE_CORRUPT;
	}
	r_layout = layout;
	return OK;
}

} // namespace gs_world_format

#endif // GAUSSIAN_SPLAT_WORLD_FORMAT_H
