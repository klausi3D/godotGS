#pragma once

// Codec-aware staged source (BV-C5b; docs/architecture/adr-beyond-vram-worlds.md D-12 "Decode
// location", §3.1, §3.4 step 5). CPU only; no GPU.
//
// A plain v3 + bit 7 world loaded for streaming keeps only its page directory. The async pack
// workers read the pages through the source's PayloadReadService, verify and decode them and
// pack 176 B PackedGaussians that equal the packing of the resident decode. The source's
// telemetry proves the path: every page was decoded on a worker thread, none on the main thread,
// and every page read went through the read service. A damaged page fails its capture with the
// gsq_* reason and is counted; the other chunks still stream.

#include "test_macros.h"

#include "gs_test_pump.h"
#include "test_gs_world_paged_chunks.h"

#include "../core/gaussian_streaming.h"
#include "../core/streaming_gsq_chunk_payload_source.h"
#include "../core/streaming_upload_pipeline.h"
#include "../renderer/gaussian_gpu_layout.h"

#include "core/io/dir_access.h"
#include "core/os/thread.h"

#include <cstring>

namespace TestGsqStagedSource {

// Loads p_path for streaming (not load_resident()).
inline Ref<GaussianSplatWorld> gss_load_staged(const String &p_path, Error &r_err) {
	ResourceFormatLoaderGaussianSplatWorld loader;
	r_err = ERR_BUG;
	Ref<GaussianSplatWorld> world = loader.load(p_path, "", &r_err);
	return world;
}

inline Ref<GsqPagedChunkPayloadSource> gss_source_of(const Ref<GaussianSplatWorld> &p_world) {
	if (p_world.is_null()) {
		return Ref<GsqPagedChunkPayloadSource>();
	}
	return Ref<GsqPagedChunkPayloadSource>(Object::cast_to<GsqPagedChunkPayloadSource>(p_world->get_chunk_payload_source().ptr()));
}

// Stored bytes of page p_page, from the page table of the file bytes.
inline uint64_t gss_page_stored_bytes(const PackedByteArray &p_bytes, uint32_t p_page) {
	const int64_t ext = int64_t(gs_world_format::kPrefixBytes);
	const int64_t entry = int64_t(TestGSWorldWriter::ww_u64_at(p_bytes, ext + 24)) + int64_t(p_page) * int64_t(gs_world_format::kPagedPageRecordBytes);
	return TestGSWorldPagedChunks::pc_u32(p_bytes, entry + 8);
}

// Waits until a pack worker prepared the upload at the head of the queue and copies its payload.
inline bool gss_take_prepared(StreamingUploadPipeline &p_uploads, Vector<PackedGaussian> &r_packed) {
	const auto prepared = TestGaussianSplatting::gs_pump_until([&]() {
		Thread::yield();
		MutexLock lock(p_uploads.pack_mutex);
		if (p_uploads.upload_queue_read_idx < p_uploads.upload_queue.size()) {
			StreamingUploadPipeline::PendingChunkUpload *job = p_uploads.upload_queue[p_uploads.upload_queue_read_idx];
			if (job && !job->packed_data.is_empty()) {
				r_packed = job->packed_data;
				return true;
			}
		}
		return false;
	});
	if (!prepared.ready()) {
		FAIL("no pack worker prepared the paged-GSQ chunk ", prepared.describe());
		return false;
	}
	return true;
}

struct GssRange {
	uint32_t start;
	uint32_t count;
	uint32_t first_page;
	uint32_t last_page;
};

} // namespace TestGsqStagedSource

TEST_CASE("[GaussianSplatting][WorldIO] paged GSQ chunks (bit 7): pack workers read pages through the PayloadReadService and decode them off the main thread (BV-C5b)") {
	using namespace TestGsqStagedSource;
	using namespace TestGSWorldPagedChunks;
	const String dir = TestGSWorldWriter::ww_dir("gsq_staged");
	const String path = dir.path_join("world.gsplatworld");
	const PCWorld w = pc_make(3u); // chunks of 9000, 0 and 5000 splats; pages 4096 + 4096 + 808, 4096 + 904
	String message;
	if (pc_write(w, gs_world_writer::SourceMode::SINGLE_PASS, path, nullptr, &message) != OK) {
		FAIL("paged write refused: ", message.utf8().get_data());
		TestGSWorldWriter::ww_remove_dir(dir);
		return;
	}
	const PackedByteArray bytes = TestGSWorldWriter::ww_read(path);
	const uint32_t high = w.spec.sh_high_order;

	// The resident decode is the reference for every packed splat.
	Error err = OK;
	Ref<GaussianSplatWorld> resident = pc_load(path, err);
	if (resident.is_null() || resident->get_gaussian_data().is_null()) {
		FAIL("the resident load of the paged world failed");
		DirAccess::remove_absolute(path);
		TestGSWorldWriter::ww_remove_dir(dir);
		return;
	}
	const LocalVector<Gaussian> &reference = resident->get_gaussian_data()->get_gaussian_storage();
	const Vector3 *reference_sh = resident->get_gaussian_data()->get_sh_high_order_coefficients_ptr();
	if (reference.size() != 14000u || reference_sh == nullptr) {
		FAIL("the resident reference payload is incomplete");
		DirAccess::remove_absolute(path);
		TestGSWorldWriter::ww_remove_dir(dir);
		return;
	}

	// The streaming load keeps the directory only: no payload, no page read yet.
	Ref<GaussianSplatWorld> world = gss_load_staged(path, err);
	CHECK(err == OK);
	Ref<GsqPagedChunkPayloadSource> source = gss_source_of(world);
	if (source.is_null()) {
		FAIL("the streaming load of a bit-7 world did not publish a GsqPagedChunkPayloadSource");
		DirAccess::remove_absolute(path);
		TestGSWorldWriter::ww_remove_dir(dir);
		return;
	}
	CHECK(world->get_gaussian_data().is_null());
	CHECK(world->is_streamable_payload());
	CHECK(source->get_page_count() == 5u);
	CHECK(source->get_count() == 14000u);
	CHECK(source->get_sh_degree() == 3u);
	CHECK(source->get_decode_stats_struct().pages_decoded == 0u);
	CHECK(source->get_decode_stats_struct().reads.submitted == 0u);

	// Placeholder resident data of the right size whose splats differ from the world's, so a pack
	// that bypassed the payload source could not reproduce the reference bytes.
	Ref<GaussianData> placeholder;
	placeholder.instantiate();
	{
		LocalVector<Gaussian> filler;
		filler.resize(14000u);
		placeholder->set_gaussians(filler);
	}
	Ref<GaussianStreamingSystem> system;
	system.instantiate();
	LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
	// Whole chunk 0 (pages 0-2), whole chunk 2 (pages 3-4) and a range across the page 0/1 boundary.
	const GssRange ranges[] = {
		{ 0u, 9000u, 0u, 2u },
		{ 9000u, 5000u, 3u, 4u },
		{ 4000u, 200u, 0u, 1u },
	};
	const uint32_t range_count = 3u;
	chunks.resize(range_count);
	for (uint32_t i = 0; i < range_count; i++) {
		chunks[i] = GaussianStreamingTypes::StreamingChunk();
		chunks[i].start_idx = ranges[i].start;
		chunks[i].count = ranges[i].count;
		chunks[i].effective_count = ranges[i].count;
		chunks[i].is_visible = true;
		chunks[i].buffer_slot = UINT32_MAX;
	}
	system->_test_begin_device_free_load_scan(placeholder, range_count);
	system->set_chunk_payload_source(0, source);
	StreamingUploadPipeline &uploads = system->_internal_get_upload_pipeline();

	uint32_t packed_rounds = 0;
	uint64_t expected_splats = 0;
	uint64_t expected_stored_bytes = 0;
	uint32_t expected_pages = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const GssRange &range = ranges[i];
		CAPTURE(i);
		uploads.start_pack_threads(*system.ptr());
		if (!uploads.pack_thread_running.load(std::memory_order_acquire)) {
			FAIL("the pack workers did not start");
			break;
		}
		system->begin_frame();
		Vector<PackedGaussian> packed;
		const bool queued = uploads.queue_chunk_load(*system.ptr(), 0, i);
		CHECK(queued);
		const bool prepared = queued && gss_take_prepared(uploads, packed);
		uploads.stop_pack_threads(*system.ptr());
		if (!prepared) {
			break;
		}
		packed_rounds++;
		for (uint32_t p = range.first_page; p <= range.last_page; p++) {
			expected_pages++;
			expected_stored_bytes += gss_page_stored_bytes(bytes, p);
			expected_splats += p == 2u ? 808u : (p == 4u ? 904u : 4096u);
		}

		// The worker's 176 B payload equals the packing of the resident decode of the same range.
		LocalVector<Gaussian> slice;
		slice.resize(range.count);
		for (uint32_t j = 0; j < range.count; j++) {
			slice[j] = reference[range.start + j];
		}
		Vector<PackedGaussian> expected;
		SHCompressionMetrics metrics;
		const bool packed_ok = pack_gaussians_range(slice, 0, range.count, expected, metrics,
				reference_sh + uint64_t(range.start) * high, w.spec.sh_first_order, high);
		CHECK(packed_ok);
		if (packed.size() != int64_t(range.count) || expected.size() != int64_t(range.count)) {
			FAIL("packed ", packed.size(), " and expected ", expected.size(), " splats for a range of ", range.count);
			continue;
		}
		CHECK(memcmp(packed.ptr(), expected.ptr(), sizeof(PackedGaussian) * range.count) == 0);
	}
	system->_test_end_device_free_load_scan();
	system.unref();

	// Telemetry: the async path ran, and nothing ran instead of it.
	const GsqPagedChunkPayloadSource::DecodeStats stats = source->get_decode_stats_struct();
	CHECK(packed_rounds == range_count);
	CHECK(stats.captures == uint64_t(range_count));
	CHECK(stats.captures_failed == 0u);
	CHECK(stats.page_failures == 0u);
	CHECK(stats.pages_decoded == uint64_t(expected_pages));
	CHECK(stats.pages_decoded_on_worker_threads == uint64_t(expected_pages));
	CHECK(stats.pages_decoded_on_main_thread == 0u);
	CHECK(stats.splats_decoded == expected_splats);
	CHECK(stats.decode_nsec > 0u);
	CHECK(stats.reads.submitted == uint64_t(expected_pages));
	CHECK(stats.reads.completed_ok == uint64_t(expected_pages));
	CHECK(stats.reads.bytes_read == expected_stored_bytes);
	CHECK(stats.stored_bytes_read == expected_stored_bytes);
	CHECK(stats.last_failure_reason.is_empty());
	const Dictionary exported = source->get_decode_stats();
	CHECK(int64_t(exported.get("pages_decoded_on_worker_threads", int64_t(-1))) == int64_t(expected_pages));
	CHECK(int64_t(exported.get("pages_decoded_on_main_thread", int64_t(-1))) == 0);

	source.unref();
	world.unref();
	resident.unref();
	DirAccess::remove_absolute(path);
	TestGSWorldWriter::ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO] paged GSQ chunks (bit 7): a damaged page fails its capture with the gsq_* reason; other chunks still stream (BV-C5b)") {
	using namespace TestGsqStagedSource;
	using namespace TestGSWorldPagedChunks;
	const String dir = TestGSWorldWriter::ww_dir("gsq_staged_damage");
	const String path = dir.path_join("world.gsplatworld");
	const PCWorld w = pc_make(0u);
	String message;
	if (pc_write(w, gs_world_writer::SourceMode::SINGLE_PASS, path, nullptr, &message) != OK) {
		FAIL("paged write refused: ", message.utf8().get_data());
		TestGSWorldWriter::ww_remove_dir(dir);
		return;
	}
	const PackedByteArray bytes = TestGSWorldWriter::ww_read(path);
	const int64_t ext = int64_t(gs_world_format::kPrefixBytes);
	const int64_t page_table = int64_t(TestGSWorldWriter::ww_u64_at(bytes, ext + 24));
	const uint64_t page1 = TestGSWorldWriter::ww_u64_at(bytes, page_table + int64_t(gs_world_format::kPagedPageRecordBytes));
	struct Damage {
		const char *name;
		int64_t offset;
		const char *reason;
	};
	const Damage damages[] = {
		{ "a byte inside page 1", int64_t(page1) + 300, "gsq_digest_mismatch" },
		{ "page 1's decoded digest in the page table", page_table + int64_t(gs_world_format::kPagedPageRecordBytes) + 48, "gsq_decoded_digest_mismatch" },
	};
	for (const Damage &d : damages) {
		INFO(d.name);
		PackedByteArray bad = bytes;
		bad.ptrw()[d.offset] ^= 0x01u;
		if (!TestGSWorldWriter::ww_write_raw(path, bad)) {
			FAIL("could not write the damaged world");
			continue;
		}
		// The directory is intact, so the streaming load succeeds and reads no page.
		Error err = OK;
		Ref<GaussianSplatWorld> world = gss_load_staged(path, err);
		Ref<GsqPagedChunkPayloadSource> source = gss_source_of(world);
		CHECK(err == OK);
		if (source.is_null()) {
			FAIL("the streaming load did not publish a GsqPagedChunkPayloadSource");
			continue;
		}
		LocalVector<Gaussian> gaussians;
		LocalVector<Vector3> sh;
		uint32_t first_order = 0;
		uint32_t high_order = 0;
		// Chunk 0 holds page 1: its capture fails visibly and returns nothing.
		CHECK_FALSE(source->capture_chunk_snapshot(0, 9000u, gaussians, sh, first_order, high_order));
		CHECK(gaussians.is_empty());
		GsqPagedChunkPayloadSource::DecodeStats stats = source->get_decode_stats_struct();
		CHECK(stats.captures_failed == 1u);
		CHECK(stats.page_failures == 1u);
		CHECK(stats.pages_decoded == 1u); // page 0, before page 1 failed
		CHECK(stats.last_failure_reason.contains(d.reason));
		CHECK(stats.last_failure_reason.contains("page 1"));
		// Chunk 2 (pages 3 and 4) is unaffected; this capture ran on the main thread and is counted so.
		CHECK(source->capture_chunk_snapshot(9000u, 5000u, gaussians, sh, first_order, high_order));
		CHECK(gaussians.size() == 5000u);
		stats = source->get_decode_stats_struct();
		CHECK(stats.captures_failed == 1u);
		CHECK(stats.pages_decoded == 3u);
		CHECK(stats.pages_decoded_on_main_thread == 3u);
		CHECK(stats.pages_decoded_on_worker_threads == 0u);
		source.unref();
		world.unref();
	}
	DirAccess::remove_absolute(path);
	TestGSWorldWriter::ww_remove_dir(dir);
}
