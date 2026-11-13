#pragma once

// Shared CPU-only producer fixture for the D3-1 store contract and adapter identity tests.
// The reference never calls PayloadPageStore or GsqPagedChunkPayloadSource: it decodes the
// actual writer output through the codec API, in the page directory's order.

#include "test_gs_world_paged_chunks.h"

#include "../core/streaming_gsq_chunk_payload_source.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/os/os.h"

#include <cstring>

namespace TestPayloadPageStore {

struct Digest {
	uint8_t bytes[gs_gsq::kDigestBytes] = {};
};

struct Fixture {
	String path;
	String error;
	bool valid = false;
	LocalVector<GsqPagedChunkPayloadSource::Page> pages;
	gs_gsq::EncodeParams codec_params;
	uint32_t sh_degree = 3u;
	uint32_t sh_first_order = 3u;
	uint32_t sh_high_order = 12u;
	uint32_t count = 14000u;
	AABB bounds;
	LocalVector<uint32_t> page_first_splat;
	LocalVector<Gaussian> reference;
	LocalVector<Vector3> reference_sh;
	LocalVector<Digest> reference_digests;
	PackedByteArray bytes;
	uint64_t total_stored_bytes = 0;

	explicit Fixture(const String &p_suffix) {
		path = OS::get_singleton()->get_temp_path().path_join("gs_page_store_" + p_suffix + "_" +
				itos(OS::get_singleton()->get_ticks_usec()) + ".gsplatworld");
		const TestGSWorldPagedChunks::PCWorld world = TestGSWorldPagedChunks::pc_make(sh_degree);
		bounds = world.spec.bounds;
		uint32_t written_pages = 0;
		if (TestGSWorldPagedChunks::pc_write(world, gs_world_writer::SourceMode::SINGLE_PASS,
					path, &written_pages, &error) != OK) {
			return;
		}
		if (written_pages != 5u) {
			error = "the deterministic writer fixture must contain five GSQ pages";
			return;
		}
		const gs_world_writer::CodecParams codec = gs_world_writer::gsq_v1_codec_params();
		if (gs_world_writer::gsq_encode_params(codec, codec_params, error) != OK) {
			return;
		}
		bytes = TestGSWorldWriter::ww_read(path);
		const uint64_t extension = gs_world_format::kPrefixBytes;
		if (uint64_t(bytes.size()) < extension + gs_world_format::kPagedExtensionBytes) {
			error = "writer output has no complete paged extension";
			return;
		}
		if (decode_uint32(bytes.ptr() + extension + 16u) != 5u) {
			error = "writer page count differs from the deterministic fixture";
			return;
		}
		const uint64_t table = decode_uint64(bytes.ptr() + extension + 24u);
		const uint64_t table_size = 5u * gs_world_format::kPagedPageRecordBytes;
		if (table > uint64_t(bytes.size()) || table_size > uint64_t(bytes.size()) - table) {
			error = "writer output has an incomplete page table";
			return;
		}
		const uint32_t expected_counts[] = { 4096u, 4096u, 808u, 4096u, 904u };
		pages.resize(5u);
		page_first_splat.resize(6u);
		reference_digests.resize(5u);
		reference.resize(count);
		reference_sh.resize(count * sh_high_order);
		gs_gsq::DecodeContext decoder;
		uint32_t first = 0;
		for (uint32_t p = 0; p < 5u; p++) {
			const uint8_t *record = bytes.ptr() + table + uint64_t(p) * gs_world_format::kPagedPageRecordBytes;
			GsqPagedChunkPayloadSource::Page &page = pages[p];
			page.offset = decode_uint64(record);
			page.stored_bytes = decode_uint32(record + 8u);
			page.splat_count = decode_uint32(record + 12u);
			memcpy(page.stored_digest, record + 16u, gs_gsq::kDigestBytes);
			memcpy(page.decoded_digest, record + 48u, gs_gsq::kDigestBytes);
			for (uint32_t i = 0; i < 6u; i++) {
				page.extent[i] = decode_uint32(record + 80u + i * 4u);
			}
			if (page.splat_count != expected_counts[p] || page.offset > uint64_t(bytes.size()) ||
					page.stored_bytes > uint64_t(bytes.size()) - page.offset) {
				error = "writer page size/count lies outside the deterministic fixture";
				return;
			}
			page_first_splat[p] = first;
			const uint8_t *stored = bytes.ptr() + page.offset;
			gs_gsq::PageInfo info;
			if (gs_gsq::verify_page(stored, page.stored_bytes, info, error) != OK) {
				return;
			}
			if (info.splat_count != page.splat_count || info.sh_degree != sh_degree ||
					memcmp(info.stored_digest, page.stored_digest, gs_gsq::kDigestBytes) != 0) {
				error = "writer page and directory disagree";
				return;
			}
			Gaussian *gaussians = reference.ptr() + first;
			Vector3 *sh = reference_sh.ptr() + uint64_t(first) * sh_high_order;
			if (decoder.decode_range(stored, page.stored_bytes, 0u, page.splat_count,
						gaussians, sh, error, true) != OK) {
				return;
			}
			gs_gsq::decoded_digest(gaussians, sh, page.splat_count, sh_degree, reference_digests[p].bytes);
			if (memcmp(reference_digests[p].bytes, page.decoded_digest, gs_gsq::kDigestBytes) != 0) {
				error = "independent codec decode disagrees with the writer's decoded digest";
				return;
			}
			total_stored_bytes += page.stored_bytes;
			first += page.splat_count;
		}
		page_first_splat[5] = first;
		if (first != count) {
			error = "independent codec decode did not cover the whole fixture";
			return;
		}
		error = String();
		valid = true;
	}

	~Fixture() {
		DirAccess::remove_absolute(path);
	}

	Fixture(const Fixture &) = delete;
	Fixture &operator=(const Fixture &) = delete;
};

} // namespace TestPayloadPageStore
