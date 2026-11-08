#pragma once

// GSQ page builder: the writer's codec stage (BV-C3; docs/architecture/adr-beyond-vram-worlds.md
// D-12, §4.4 W12/W15, §6.2 "Per-chunk order").
//
// It turns one chunk, whose splats arrive in importance order (§6.2: a prefix of the chunk is a
// valid sample), into validated GSQ v1 pages:
//
//   1. Page constants. The codec stores area, DC alpha, stroke age, brush axes, painterly_meta
//      and render_meta once per page and refuses a page whose splats differ in them
//      (gsq_heterogeneous_field). The builder therefore partitions the chunk into constant
//      groups, splats whose page constants are bit-identical, keeping importance order inside
//      each group. A chunk that needs more than one group is counted (heterogeneous_chunks,
//      constant_groups), never silently refused or rewritten.
//   2. Page order follows importance (user decision 2026-11-08). Each group is cut into pages of
//      at most kWriterPageSplats consecutive splats of its importance order, and the pages of
//      the chunk are ordered by their first (most important) splat. A prefix of pages is
//      therefore a prefix of every group: prefix LOD stays valid, at whole-page granularity.
//   3. Morton order only inside a page (decision 2026-11-07, #1347), where the codec delta-codes
//      positions. Keys are 3 x 21 bits of the order-preserving integer image of the position
//      floats relative to the page minimum, ties broken by importance rank. Integer only, so the
//      order cannot depend on -ffast-math or contraction (risk K10).
//   4. W12: every page is encoded, its stored digest verified, decoded in full and semantically
//      validated against its source splats (finite fields, every attribute within its D-12
//      quantization step, page constants bit-identical) before it is handed out.
//   5. W15: each page carries its stored digest and its decoded canonical digest.
//
// A page that still cannot be built (a field the codec refuses: non-finite, out of range, SH
// degree 4) fails the whole chunk with the gsq_* reason and the chunk and page it hit; no page
// of that chunk is returned. Nothing is dropped.
//
// Container placement (plain v3 bit 7) is BV-C5a; this stage produces the pages it will write.
// Not thread-safe: one builder per thread (it owns a DecodeContext).

#include "gsq_codec.h"

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

#include <cstdint>

namespace gs_gsq {

// One page is one zstd sub-block: the importance prefix is cut in steps of this many splats.
constexpr uint32_t kWriterPageSplats = kSubBlockSplats;

// W15: one entry per page.
struct PageRecord {
	uint32_t chunk_index = 0;
	uint32_t page_in_chunk = 0;
	uint32_t constant_group = 0; // ordinal of the page's constant group in the chunk, by first splat
	uint32_t splat_count = 0;
	uint32_t stored_bytes = 0;
	uint32_t first_source = 0; // smallest importance rank (index in the chunk as appended) in the page
	uint32_t last_source = 0; // largest
	uint8_t stored_digest[kDigestBytes] = {};
	uint8_t decoded_digest[kDigestBytes] = {};
};

struct BuiltPage {
	LocalVector<uint8_t> bytes;
	// Page slot -> index of that splat in the chunk as appended (its importance rank).
	LocalVector<uint32_t> source_index;
	PageRecord record;
};

struct PageBuildStats {
	uint64_t chunks = 0;
	uint64_t splats = 0;
	uint64_t pages = 0;
	uint64_t stored_bytes = 0;
	uint64_t constant_groups = 0; // summed over chunks; equals chunks when every chunk is homogeneous
	uint64_t heterogeneous_chunks = 0; // chunks that needed more than one constant group
};

// The parameter ranges encode_page() accepts, checked up front with a gsq_invalid_params reason.
Error validate_encode_params(const EncodeParams &p_params, String &r_reason);

class PageBuilder {
public:
	// Validates the parameters the pages will be encoded with. Must succeed before build_chunk().
	Error configure(const EncodeParams &p_params, uint32_t p_sh_degree, String &r_reason);

	// Appends the pages of one chunk to r_pages (all or nothing). p_sh_high holds
	// sh_high_order_for_degree(degree) Vector3 per splat and may be null when that is 0.
	// An empty chunk produces no page.
	Error build_chunk(uint32_t p_chunk_index, const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count,
			LocalVector<BuiltPage> &r_pages, String &r_reason);

	const PageBuildStats &get_stats() const { return stats; }

#ifdef TESTS_ENABLED
	// Runs on every encoded page before W12 sees it, so a test can prove W12 refuses a damaged page.
	using PageTestHook = void (*)(LocalVector<uint8_t> &r_page);
	static PageTestHook post_encode_test_hook;
#endif

private:
	Error _validate_page(const uint8_t *p_page, uint32_t p_page_bytes, const Gaussian *p_source, const Vector3 *p_source_sh,
			uint32_t p_count, PageRecord &r_record, String &r_reason);

	bool configured = false;
	EncodeParams params;
	uint32_t sh_degree = 0;
	uint32_t sh_high_order = 0;
	PageBuildStats stats;
	DecodeContext decoder;
	LocalVector<Gaussian> page_gaussians;
	LocalVector<Vector3> page_sh;
	LocalVector<Gaussian> decoded_gaussians;
	LocalVector<Vector3> decoded_sh;
};

} // namespace gs_gsq
