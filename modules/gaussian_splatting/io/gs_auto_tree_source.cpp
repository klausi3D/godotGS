#include "gs_auto_tree_source.h"

#include "../core/gaussian_splat_world.h"
#include "../core/streaming_gsq_chunk_payload_source.h"
#include "gaussian_splat_world_format.h"
#include "gaussian_splat_world_io.h"

#include "core/io/file_access.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

Error GsqAutoTreeSource::fail(Error p_error, const String &p_reason, String *r_error) const {
	if (r_error) {
		*r_error = p_reason;
	}
	MutexLock lock(stats_mutex);
	++stats.failures;
	stats.last_reason = p_reason;
	return p_error;
}

Error GsqAutoTreeSource::open(const String &p_path, String *r_error, uint64_t p_max_ram_bytes) {
	file_path = String();
	file_bytes = 0;
	info = {};
	pages.reset();
	sh_degree = 0;
	mode_2d = false;
	bounds = AABB();
	reserved_bytes = 0;
	open_peak_reserved_bytes = 0;
	decode_scratch_bytes = 0;
	{
		MutexLock lock(stats_mutex);
		stats = {};
	}
	if (r_error) {
		*r_error = String();
	}
	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &error);
	if (file.is_null()) {
		return fail(error == OK ? ERR_FILE_CANT_OPEN : error, "gsq_input_open_failed", r_error);
	}
	if (file->get_length() < 12u) {
		return fail(ERR_FILE_CORRUPT, "gsq_bad_header: truncated prefix", r_error);
	}
	const uint32_t magic = file->get_32();
	const uint32_t version = file->get_32();
	const uint32_t flags = file->get_32();
	if (magic != gs_world_format::kMagic || version != gs_world_format::kVersion ||
			(flags & gs_world_format::kFlagPagedChunks) == 0u || (flags & gs_world_format::kFlagHasHlod) != 0u) {
		return fail(ERR_UNAVAILABLE, "input_requires_plain_v3_gsq", r_error);
	}
	gs_world_format::HeaderLayout layout;
	if (gs_world_format::select_header(version, flags, file->get_length(), layout) != OK) {
		return fail(ERR_FILE_CORRUPT, "gsq_bad_header", r_error);
	}
	const uint32_t splat_count = file->get_32();
	file->seek(52u);
	const uint32_t chunk_count = file->get_32();
	file->seek(96u);
	const uint64_t metadata_bytes = file->get_64();
	file->seek(gs_world_format::kPrefixBytes + 16u);
	const uint32_t page_count = file->get_32();
	file->get_32(); // alignment; the canonical loader validates it below
	const uint64_t page_table_offset = file->get_64();
	// The canonical directory-only loader owns chunk/page directory copies and
	// parsed metadata, without per-splat index lists. Admit that work before it
	// allocates; nothing resident is decoded here. Metadata gets a conservative
	// parser expansion allowance, not merely its encoded byte count.
	uint64_t open_bytes = uint64_t(chunk_count) * 1024u +
			uint64_t(page_count) * 1024u + 1024u * 1024u;
	if (metadata_bytes > (UINT64_MAX - open_bytes) / 128u) {
		return fail(ERR_OUT_OF_MEMORY, "gsq_directory_ram_limit", r_error);
	}
	open_bytes += metadata_bytes * 128u;
	if (open_bytes > p_max_ram_bytes) {
		return fail(ERR_OUT_OF_MEMORY, "gsq_directory_ram_limit", r_error);
	}
	open_peak_reserved_bytes = open_bytes;
	ResourceFormatLoaderGaussianSplatWorld loader;
	Ref<GaussianSplatWorld> world = loader.load_paged_directory_for_auto_tree(p_path, &error);
	if (error != OK || world.is_null()) {
		return fail(error == OK ? ERR_FILE_CORRUPT : error, "gsq_header_or_directory_invalid", r_error);
	}
	Ref<GsqPagedChunkPayloadSource> validated_source = world->get_chunk_payload_source();
	if (validated_source.is_null() || validated_source->get_page_count() != page_count ||
			world->get_splat_count() != splat_count || page_count == 0u) {
		return fail(ERR_FILE_CORRUPT, "gsq_directory_mismatch", r_error);
	}
	const uint32_t degree = world->get_sh_degree();
	gs_hlod::TiledBakeSourceInfo source_info;
	source_info.splat_count = splat_count;
	source_info.chunk_count = page_count; // each page is a bounded, contiguous page run
	source_info.sh_first_order_count = world->get_sh_first_order_count();
	source_info.sh_high_order_count = world->get_sh_high_order_count();
	const AABB source_bounds = world->get_bounds();
	const bool source_2d = world->get_2d_mode();
	// Drop the loader's temporary directory before allocating our own.
	validated_source.unref();
	world.unref();

	file->seek(gs_world_format::kPrefixBytes + 4u);
	gs_gsq::EncodeParams params;
	for (int axis = 0; axis < 3; ++axis) {
		params.sh_bits[axis] = file->get_8();
	}
	params.opacity_bits = file->get_8();
	params.pos_step_m = file->get_float();
	params.zstd_level = int32_t(file->get_32());
	LocalVector<Page> directory;
	if (!gs_hlod::fallible_resize(directory, page_count, "gs_auto_tree_source directory")) {
		return fail(ERR_OUT_OF_MEMORY, "gsq_directory_allocation_failed", r_error);
	}
	file->seek(page_table_offset);
	uint64_t cursor = 0;
	uint32_t largest_page = 0;
	for (uint32_t p = 0; p < page_count; ++p) {
		Page &page = directory[p];
		page.offset = file->get_64();
		page.stored_bytes = file->get_32();
		page.chunk.first_splat = uint32_t(cursor);
		page.chunk.splat_count = file->get_32();
		if (file->get_buffer(page.stored_digest, gs_gsq::kDigestBytes) != gs_gsq::kDigestBytes ||
				file->get_buffer(page.decoded_digest, gs_gsq::kDigestBytes) != gs_gsq::kDigestBytes) {
			return fail(ERR_FILE_CORRUPT, "gsq_directory_short_read", r_error);
		}
		for (int component = 0; component < 6; ++component) {
			page.extent[component] = file->get_32();
		}
		file->get_32(); // owning world chunk, already validated by the loader
		file->get_32(); // reserved
		cursor += page.chunk.splat_count;
		if (page.chunk.splat_count == 0u || page.chunk.splat_count > gs_gsq::kMaxPageSplats || cursor > splat_count ||
				page.stored_bytes < gs_gsq::kPageHeaderBytes || page.offset > file->get_length() ||
				page.stored_bytes > file->get_length() - page.offset) {
			return fail(ERR_FILE_CORRUPT, "gsq_directory_mismatch", r_error);
		}
		Vector3 lower, size;
		for (int axis = 0; axis < 3; ++axis) {
			const uint32_t lo_bits = page.extent[axis];
			const uint32_t hi_bits = page.extent[axis + 3];
			// Match the codec's Q24 representability bound (|position| < 2^38).
			// Bit tests also reject nonfinite values under fast-math. Staying in
			// this range proves all outward bounds and their sizes remain finite.
			constexpr uint32_t max_position_exponent = (127u + 38u) << 23u;
			if ((lo_bits & 0x7f800000u) >= max_position_exponent || (hi_bits & 0x7f800000u) >= max_position_exponent) {
				return fail(ERR_FILE_CORRUPT, "gsq_invalid_page_extent", r_error);
			}
			float lo, hi;
			std::memcpy(&lo, &lo_bits, sizeof(lo));
			std::memcpy(&hi, &hi_bits, sizeof(hi));
			if (lo > hi) {
				return fail(ERR_FILE_CORRUPT, "gsq_invalid_page_extent", r_error);
			}
			// GSQ rounds coordinates to Q24 before dequantization. Include that
			// rounding plus outward real_t rounding and AABB size/addition rounding.
			const double margin = std::ldexp(1.0, -24);
			lower[axis] = std::nextafter(real_t(double(lo) - margin), -std::numeric_limits<real_t>::infinity());
			const real_t upper = std::nextafter(real_t(double(hi) + margin), std::numeric_limits<real_t>::infinity());
			const double span = double(upper) - double(lower[axis]);
			if (!(span < double(std::numeric_limits<real_t>::max()))) {
				return fail(ERR_FILE_CORRUPT, "gsq_invalid_page_extent", r_error);
			}
			size[axis] = std::nextafter(real_t(span), std::numeric_limits<real_t>::infinity());
		}
		page.chunk.centre_bounds = AABB(lower, size);
		largest_page = MAX(largest_page, page.stored_bytes);
	}
	if (cursor != splat_count || file->get_error() != OK) {
		return fail(ERR_FILE_CORRUPT, "gsq_directory_mismatch", r_error);
	}
	file_path = p_path;
	file_bytes = file->get_length();
	info = source_info;
	pages = std::move(directory);
	codec_params = params;
	sh_degree = degree;
	mode_2d = source_2d;
	bounds = source_bounds;
	reserved_bytes = uint64_t(page_count) * sizeof(Page) * 2u + uint64_t(p_path.length() + 1) * sizeof(char32_t) + sizeof(*this);
	// The codec retains one sub-block, not an entire decoded page: at degree 3
	// its raw scratch is <= 4096 * (9 position + 14 core + 45 SH + 6 normal)
	// = 303104 bytes, plus 15 KiB of lookup tables. ZSTD_decompressDCtx uses
	// the non-streaming context and caller-provided output, without allocating
	// an input/window buffer. 16 MiB conservatively covers that context, raw
	// allocation/probe overlap, tables, digest workspace and call-local objects.
	decode_scratch_bytes = uint64_t(largest_page) + 16u * 1024u * 1024u;
	{
		MutexLock lock(stats_mutex);
		stats.page_count = page_count;
	}
	return OK;
}

gs_hlod::TiledBakeSourceInfo GsqAutoTreeSource::get_info() const {
	return info;
}

Error GsqAutoTreeSource::get_chunk_info(uint32_t p_chunk, gs_hlod::TiledBakeChunkInfo &r_info, String *r_error) const {
	r_info = {};
	if (p_chunk >= pages.size()) {
		return fail(ERR_INVALID_PARAMETER, "gsq_chunk_out_of_range", r_error);
	}
	r_info = pages[p_chunk].chunk;
	if (r_error) {
		*r_error = String();
	}
	return OK;
}

Error GsqAutoTreeSource::read_chunk(uint32_t p_chunk, gs_hlod::TiledBakeChunk &r_chunk, String *r_error) const {
	r_chunk = {};
	if (p_chunk >= pages.size()) {
		return fail(ERR_INVALID_PARAMETER, "gsq_chunk_out_of_range", r_error);
	}
	const Page &page = pages[p_chunk];
	Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::READ);
	if (file.is_null()) {
		return fail(ERR_FILE_CANT_READ, "gsq_page_open_failed", r_error);
	}
	if (file->get_length() != file_bytes) {
		return fail(ERR_FILE_CORRUPT, "gsq_input_size_changed", r_error);
	}
	LocalVector<uint8_t> stored;
	if (!gs_hlod::fallible_resize(stored, page.stored_bytes, "gs_auto_tree_source stored page")) {
		return fail(ERR_OUT_OF_MEMORY, "gsq_page_allocation_failed", r_error);
	}
	file->seek(page.offset);
	if (file->get_buffer(stored.ptr(), page.stored_bytes) != page.stored_bytes) {
		return fail(ERR_FILE_CORRUPT, "gsq_page_short_read", r_error);
	}
	file.unref();
	gs_gsq::PageInfo decoded_info;
	String reason;
	if (gs_gsq::verify_page(stored.ptr(), page.stored_bytes, decoded_info, reason) != OK) {
		return fail(ERR_FILE_CORRUPT, reason, r_error);
	}
	bool matches = decoded_info.splat_count == page.chunk.splat_count && decoded_info.stored_bytes == page.stored_bytes &&
			decoded_info.sh_degree == sh_degree && decoded_info.opacity_bits == codec_params.opacity_bits &&
			std::memcmp(decoded_info.stored_digest, page.stored_digest, gs_gsq::kDigestBytes) == 0 &&
			std::memcmp(decoded_info.position_min, page.extent, sizeof(float) * 3u) == 0 &&
			std::memcmp(decoded_info.position_max, page.extent + 3, sizeof(float) * 3u) == 0;
	for (int band = 0; band < 3; ++band) {
		matches = matches && decoded_info.sh_bits[band] == codec_params.sh_bits[band];
	}
	if (!matches) {
		return fail(ERR_FILE_CORRUPT, "gsq_page_mismatch", r_error);
	}
	gs_hlod::TiledBakeChunk output;
	if (!gs_hlod::fallible_resize(output.gaussians, page.chunk.splat_count, "gs_auto_tree_source decoded gaussians") ||
			!gs_hlod::fallible_resize(output.sh_high_order, uint64_t(page.chunk.splat_count) * info.sh_high_order_count, "gs_auto_tree_source decoded SH")) {
		return fail(ERR_OUT_OF_MEMORY, "gsq_decoded_allocation_failed", r_error);
	}
	Error decoded;
	{
		gs_gsq::DecodeContext decoder; // Owns its ZSTD_DCtx and scratch for this call only.
		decoded = decoder.decode_range(stored.ptr(), page.stored_bytes, 0, page.chunk.splat_count,
				output.gaussians.ptr(), output.sh_high_order.is_empty() ? nullptr : output.sh_high_order.ptr(), reason, false);
	}
	stored.reset();
	if (decoded != OK) {
		return fail(decoded == ERR_OUT_OF_MEMORY ? decoded : ERR_FILE_CORRUPT, reason, r_error);
	}
	uint8_t digest[gs_gsq::kDigestBytes];
	gs_gsq::decoded_digest(output.gaussians.ptr(), output.sh_high_order.is_empty() ? nullptr : output.sh_high_order.ptr(),
			page.chunk.splat_count, sh_degree, digest);
	{
		MutexLock lock(stats_mutex);
		++stats.pages_decoded;
		++stats.pages_decoded_digest_checked;
	}
	if (std::memcmp(digest, page.decoded_digest, gs_gsq::kDigestBytes) != 0) {
		return fail(ERR_FILE_CORRUPT, "gsq_decoded_digest_mismatch", r_error);
	}
	const Vector3 upper = page.chunk.centre_bounds.get_end();
	for (const Gaussian &gaussian : output.gaussians) {
		for (int axis = 0; axis < 3; ++axis) {
			if (!(gaussian.position[axis] >= page.chunk.centre_bounds.position[axis] && gaussian.position[axis] <= upper[axis])) {
				return fail(ERR_FILE_CORRUPT, "gsq_decoded_extent_mismatch", r_error);
			}
		}
	}
	r_chunk = std::move(output);
	if (r_error) {
		*r_error = String();
	}
	return OK;
}

GsqAutoTreeSource::Stats GsqAutoTreeSource::get_stats() const {
	MutexLock lock(stats_mutex);
	return stats;
}
