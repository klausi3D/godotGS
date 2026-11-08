#ifndef GAUSSIAN_SPLAT_WORLD_FORMAT_H
#define GAUSSIAN_SPLAT_WORLD_FORMAT_H

#include "core/error/error_list.h"

#include <cstdint>

// Header selection shared by the runtime loader and editor importer. This is
// explicit format identity; payload contents never select a different schema.
namespace gs_world_format {

static constexpr uint32_t kMagic = 0x57505347u; // 'GSPW' little-endian.
static constexpr uint32_t kVersionV1 = 1u;
static constexpr uint32_t kVersionV2 = 2u;
static constexpr uint32_t kVersion = 3u;
// Plain-v3 header flag bits (bits 0-5), shared by the in-core saver/loader
// (gaussian_splat_world_io.cpp) and the streaming writer (gs_world_writer.cpp).
static constexpr uint32_t kFlagHasMetadata = 1u << 0u;
static constexpr uint32_t kFlagIs2D = 1u << 1u;
static constexpr uint32_t kFlagHasChunks = 1u << 2u;
static constexpr uint32_t kFlagHasHighSh = 1u << 3u;
static constexpr uint32_t kFlagCompressed = 1u << 4u;
static constexpr uint32_t kFlagResidentPayload = 1u << 5u;
static constexpr uint32_t kFlagHasHlod = 1u << 6u;
// Bit 7 (BV-C5a, ADR D-12 "Containers"): the payload is a run of GSQ v1 pages per chunk
// instead of raw Gaussian + SH sections. Readers built before it refuse the file, because
// select_header() refuses every flag it does not know (ERR_FILE_UNRECOGNIZED).
static constexpr uint32_t kFlagPagedChunks = 1u << 7u;
static constexpr uint32_t kPlainFlags = (1u << 6u) - 1u;
static constexpr uint64_t kLegacyPrefixBytes = 104u;
static constexpr uint64_t kPrefixBytes = 112u;
static constexpr uint64_t kHlodExtensionBytes = 80u;
// One plain chunk-table record: bounds position, bounds size, centre (3 x vec3 f32),
// radius f32, indices_offset u64 (in indices, not bytes), index_count u32, reserved u32.
static constexpr uint64_t kChunkRecordBytes = 56u;

// Plain v3 + bit 7 (paged GSQ chunks). File layout:
//
//   prefix (112 B, the plain v3 header; sh_offset and indices_offset are 0, gaussian_offset is
//          the first page, chunk_table_offset the paged chunk table)
//   paged extension (64 B, below)
//   zero padding up to kPagedPageAlignment
//   pages: GSQ v1 pages (io/gsq/gsq_codec.h) in chunk order, each starting on a
//          kPagedPageAlignment boundary, zero padding between them
//   paged chunk table: chunk_count x kPagedChunkRecordBytes
//   page table: page_count x kPagedPageRecordBytes
//   metadata (optional, as plain v3)
//
// Paged extension, little endian:
//   0  u32   codec id (1 = GSQ v1)
//   4  u8[3] SH bits per band, u8 opacity bits      } the W16 codec block, as the writer
//   8  f32   pos_step_m                              } was given it
//   12 i32   zstd level                              }
//   16 u32   page count
//   20 u32   page alignment (kPagedPageAlignment)
//   24 u64   page table offset
//   32 u8[32] reserved, zero
//
// Paged chunk record: bounds position, bounds size, centre (3 x vec3 f32), radius f32,
// first page u32, page count u32, splat count u32, reserved u32 (zero). A chunk's splats are the
// concatenation of its pages; the index lists of the uncompressed container do not exist.
//
// Page record: offset u64, stored bytes u32, splat count u32, stored SHA-256 [32],
// decoded-canonical SHA-256 [32], position extent min xyz / max xyz (6 x f32, equal to the
// page header's), chunk index u32, reserved u32 (zero).
static constexpr uint64_t kPagedExtensionBytes = 64u;
static constexpr uint64_t kPagedPageAlignment = 4096u;
static constexpr uint64_t kPagedChunkRecordBytes = 56u;
static constexpr uint64_t kPagedPageRecordBytes = 112u;

struct HeaderLayout {
	uint64_t prefix_bytes = 0u;
	uint64_t header_bytes = 0u;
	bool has_hlod = false;
	bool has_layout_word = false;
	bool has_paged_chunks = false; // bit 7: header_bytes includes kPagedExtensionBytes
};

inline Error select_header(uint32_t p_version, uint32_t p_flags, uint64_t p_file_bytes, HeaderLayout &r_layout) {
	HeaderLayout layout;
	if (p_version != kVersionV1 && p_version != kVersionV2 && p_version != kVersion) {
		return ERR_FILE_CORRUPT;
	}
	layout.has_hlod = (p_flags & kFlagHasHlod) != 0u;
	layout.has_paged_chunks = (p_flags & kFlagPagedChunks) != 0u;
	if ((p_flags & ~(kPlainFlags | kFlagHasHlod | kFlagPagedChunks)) != 0u) {
		return layout.has_hlod ? ERR_FILE_CORRUPT : ERR_FILE_UNRECOGNIZED;
	}
	if (layout.has_paged_chunks) {
		// Bit 7 exists only in plain v3, never with HLOD, the whole-section gzip (bit 4) or the
		// resident-payload marker (bit 5), and always with its chunk table (bit 2).
		if (p_version != kVersion || layout.has_hlod || (p_flags & (kFlagCompressed | kFlagResidentPayload)) != 0u ||
				(p_flags & kFlagHasChunks) == 0u) {
			return ERR_FILE_CORRUPT;
		}
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
	layout.header_bytes = layout.prefix_bytes + (layout.has_hlod ? kHlodExtensionBytes : 0u) +
			(layout.has_paged_chunks ? kPagedExtensionBytes : 0u);
	if (p_file_bytes < layout.header_bytes) {
		return ERR_FILE_CORRUPT;
	}
	r_layout = layout;
	return OK;
}

} // namespace gs_world_format

#endif // GAUSSIAN_SPLAT_WORLD_FORMAT_H
