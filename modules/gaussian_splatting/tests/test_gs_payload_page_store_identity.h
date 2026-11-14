#pragma once

// D3-1's "before" evidence. These cases exercise the existing chunk source, with no
// PayloadPageStore dependency. Their codec reference and expectations survive extraction.

#include "test_macros.h"

#include "gs_payload_page_store_test_fixture.h"

#include "../renderer/gaussian_gpu_layout.h"

#include "core/crypto/crypto_core.h"
#include "core/os/thread.h"

#include <cstring>
#include <string>

namespace TestPayloadPageStoreIdentity {

using TestPayloadPageStore::Digest;
using TestPayloadPageStore::Fixture;

struct Range {
	uint32_t first = 0;
	uint32_t count = 0;
	bool indexed = false;
};

// A duplicate index and reversed page order exercise output scatter and page de-duplication.
static constexpr uint32_t indexed_splats[] = { 13999u, 0u, 4096u, 4096u, 8192u, 9000u, 13096u, 3u };

inline LocalVector<Range> ranges(const Fixture &p_fixture) {
	LocalVector<Range> result;
	for (uint32_t p = 0; p < 5u; p++) {
		result.push_back({ p_fixture.page_first_splat[p], p_fixture.pages[p].splat_count, false });
	}
	result.push_back({ 0u, 9000u, false }); // the first complete chunk
	result.push_back({ 9000u, 0u, false }); // the empty middle chunk
	result.push_back({ 9000u, 5000u, false }); // the final complete chunk
	result.push_back({ 4000u, 200u, false }); // page 0 -> page 1
	result.push_back({ 8900u, 200u, false }); // page 2 -> page 3, across a chunk boundary
	result.push_back({ 0u, 8u, true });
	return result;
}

struct Capture {
	bool ok = false;
	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh;
	uint32_t first_order = 0;
	uint32_t high_order = 0;
	uint64_t decode_nsec = 0;
	uint64_t second_take_nsec = 0;
};

struct CaptureRun {
	Ref<GsqPagedChunkPayloadSource> source;
	LocalVector<Range> work;
	LocalVector<Capture> captures;
	bool ran_on_main_thread = false;

	static void run(void *p_user) {
		CaptureRun *self = static_cast<CaptureRun *>(p_user);
		self->ran_on_main_thread = Thread::is_main_thread();
		self->captures.resize(self->work.size());
		for (uint32_t i = 0; i < self->work.size(); i++) {
			const Range &range = self->work[i];
			Capture &capture = self->captures[i];
			if (range.indexed) {
				capture.ok = self->source->capture_indexed_chunk_snapshot(indexed_splats, range.count,
						capture.gaussians, capture.sh, capture.first_order, capture.high_order);
			} else {
				capture.ok = self->source->capture_chunk_snapshot(range.first, range.count,
						capture.gaussians, capture.sh, capture.first_order, capture.high_order);
			}
			capture.decode_nsec = self->source->take_thread_last_capture_decode_nsec();
			capture.second_take_nsec = self->source->take_thread_last_capture_decode_nsec();
		}
	}
};

inline bool check_payload(const Fixture &p_fixture, const Range &p_range, const Capture &p_capture) {
	if (!p_capture.ok) {
		FAIL("the unchanged chunk adapter refused a deterministic capture");
		return false;
	}
	if (p_capture.gaussians.size() != p_range.count ||
			p_capture.sh.size() != p_range.count * p_fixture.sh_high_order) {
		FAIL("capture cardinality differs from the independent codec reference");
		return false;
	}
	CHECK(p_capture.first_order == p_fixture.sh_first_order);
	CHECK(p_capture.high_order == p_fixture.sh_high_order);
	CHECK(p_capture.second_take_nsec == 0u);
	if (p_range.count == 0u) {
		CHECK(p_capture.decode_nsec == 0u);
		return true;
	}
	CHECK(p_capture.decode_nsec > 0u);
	LocalVector<Gaussian> expected;
	LocalVector<Vector3> expected_sh;
	expected.resize(p_range.count);
	expected_sh.resize(p_range.count * p_fixture.sh_high_order);
	for (uint32_t i = 0; i < p_range.count; i++) {
		const uint32_t index = p_range.indexed ? indexed_splats[i] : p_range.first + i;
		expected[i] = p_fixture.reference[index];
		for (uint32_t k = 0; k < p_fixture.sh_high_order; k++) {
			expected_sh[i * p_fixture.sh_high_order + k] = p_fixture.reference_sh[index * p_fixture.sh_high_order + k];
		}
	}
	Digest expected_decoded;
	Digest actual_decoded;
	gs_gsq::decoded_digest(expected.ptr(), expected_sh.ptr(), p_range.count, p_fixture.sh_degree, expected_decoded.bytes);
	gs_gsq::decoded_digest(p_capture.gaussians.ptr(), p_capture.sh.ptr(), p_range.count,
			p_fixture.sh_degree, actual_decoded.bytes);
	CHECK(memcmp(actual_decoded.bytes, expected_decoded.bytes, gs_gsq::kDigestBytes) == 0);

	// Pin the actual 176-byte upload payload as well as the canonical decoded fields. The
	// reference inputs above came directly from the codec, never from the adapter being tested.
	Vector<PackedGaussian> expected_packed;
	Vector<PackedGaussian> actual_packed;
	SHCompressionMetrics expected_metrics;
	SHCompressionMetrics actual_metrics;
	if (!pack_gaussians_range(expected, 0u, p_range.count, expected_packed, expected_metrics,
				expected_sh.ptr(), p_fixture.sh_first_order, p_fixture.sh_high_order) ||
			!pack_gaussians_range(p_capture.gaussians, 0u, p_range.count, actual_packed, actual_metrics,
					p_capture.sh.ptr(), p_fixture.sh_first_order, p_fixture.sh_high_order)) {
		FAIL("CPU packing could not produce both payloads for the identity comparison");
		return false;
	}
	if (expected_packed.size() != int64_t(p_range.count) || actual_packed.size() != int64_t(p_range.count)) {
		FAIL("CPU packing produced an incomplete identity payload");
		return false;
	}
	const size_t payload_bytes = size_t(p_range.count) * sizeof(PackedGaussian);
	Digest expected_payload;
	Digest actual_payload;
	if (CryptoCore::sha256(reinterpret_cast<const uint8_t *>(expected_packed.ptr()), payload_bytes, expected_payload.bytes) != OK ||
			CryptoCore::sha256(reinterpret_cast<const uint8_t *>(actual_packed.ptr()), payload_bytes, actual_payload.bytes) != OK) {
		FAIL("SHA-256 could not hash both independently produced packed payloads");
		return false;
	}
	CHECK(memcmp(actual_payload.bytes, expected_payload.bytes, gs_gsq::kDigestBytes) == 0);
	CHECK(memcmp(actual_packed.ptr(), expected_packed.ptr(), payload_bytes) == 0);
	return true;
}

inline void check_stats(const Fixture &p_fixture, const GsqPagedChunkPayloadSource &p_source,
		bool p_digest_checked, bool p_worker) {
	// Five page captures, three chunk captures (one empty), two crossing ranges, one indexed
	// capture. This is independent of the adapter's telemetry and completion order.
	constexpr uint64_t expected_captures = 11u;
	constexpr uint64_t expected_pages = 19u;
	constexpr uint64_t expected_splats = 55096u;
	const uint64_t expected_bytes = 4u * p_fixture.total_stored_bytes - p_fixture.pages[4].stored_bytes;
	const GsqPagedChunkPayloadSource::DecodeStats stats = p_source.get_decode_stats_struct();
	CHECK(stats.captures == expected_captures);
	CHECK(stats.captures_failed == 0u);
	CHECK(stats.pages_decoded == expected_pages);
	CHECK(stats.splats_decoded == expected_splats);
	CHECK(stats.pages_decoded_digest_checked == (p_digest_checked ? expected_pages : 0u));
	CHECK(stats.pages_decoded_on_worker_threads == (p_worker ? expected_pages : 0u));
	CHECK(stats.pages_decoded_on_main_thread == (p_worker ? 0u : expected_pages));
	CHECK(stats.page_failures == 0u);
	CHECK(stats.stored_bytes_read == expected_bytes);
	CHECK(stats.last_failure_reason.is_empty());
	CHECK(stats.decode_nsec > 0u);
	CHECK(stats.verify_nsec > 0u); // the old adapter exposes verify duration, not a verify count
	CHECK(stats.verify_nsec + stats.digest_nsec <= stats.decode_nsec);
	if (p_digest_checked) {
		CHECK(stats.digest_nsec > 0u);
	} else {
		CHECK(stats.digest_nsec == 0u);
	}
	CHECK(stats.reads.submitted == expected_pages);
	CHECK(stats.reads.completed_ok == expected_pages);
	CHECK(stats.reads.completed_failed == 0u);
	CHECK(stats.reads.refused_busy == 0u);
	CHECK(stats.reads.bytes_read == expected_bytes);
	CHECK(stats.reads.handle_opens == expected_pages);
	CHECK(stats.reads.handle_reuses == 0u);
	CHECK(stats.reads.open_handles == 0u);
	CHECK(stats.reads.bytes_in_flight == 0u);
	CHECK(stats.reads.queued == 0u);
	CHECK(stats.reads.reading == 0u);
	CHECK(stats.reads.ready == 0u);

	const Dictionary exported = p_source.get_decode_stats();
	// Freeze the tests-enabled API, including D3-5/D3-6's explicit additions. Any
	// future key addition or removal requires a compatibility decision here.
	CHECK(exported.size() == 48);
	const char *const keys[] = {
		"page_count", "captures", "captures_failed", "pages_decoded", "splats_decoded",
		"decode_nsec", "digest_nsec", "verify_nsec", "decoded_digest_verified",
		"pages_decoded_digest_checked", "pages_decoded_on_worker_threads",
		"pages_decoded_on_main_thread", "page_failures", "stored_bytes_read",
		"last_failure_reason", "reads_submitted", "reads_completed_ok",
		"reads_completed_failed", "reads_refused_busy", "read_bytes", "read_handle_opens",
		"route", "source_generation", "collection_epoch", "transport_generation",
		"stage_time_scope", "read_time_scope", "verify_scope",
		"captures_on_worker_threads", "captures_on_main_thread",
		"pages_decoded_digest_skipped", "digest_status", "decode_remainder_nsec",
		"read_requests", "read_latency_nsec", "read_queue_nsec", "read_failures",
		"test_decode_delay_calls", "test_decode_delay_nsec",
		"test_read_delay_calls", "test_read_delay_nsec",
		"digest_verification_failures", "cancellations",
		"retained_completion_peak_bytes", "retained_completion_peak_scope",
		"stored_stash_bytes_peak", "stored_stash_bytes_peak_scope", "queue_observations",
	};
	for (const char *key : keys) {
		CHECK_MESSAGE(exported.has(key), "missing compatibility telemetry key: ", std::string(key));
	}
	CHECK(String(exported.get("route", "")) == "chunk");
	CHECK(int64_t(exported.get("collection_epoch", int64_t(0))) == int64_t(uint64_t(p_source.get_instance_id())));
	CHECK(int64_t(exported.get("page_count", int64_t(-1))) == 5);
	CHECK(int64_t(exported.get("captures", int64_t(-1))) == int64_t(expected_captures));
	CHECK(int64_t(exported.get("captures_failed", int64_t(-1))) == 0);
	CHECK(int64_t(exported.get("pages_decoded", int64_t(-1))) == int64_t(expected_pages));
	CHECK(int64_t(exported.get("splats_decoded", int64_t(-1))) == int64_t(expected_splats));
	CHECK(int64_t(exported.get("pages_decoded_digest_checked", int64_t(-1))) == int64_t(p_digest_checked ? expected_pages : 0u));
	CHECK(int64_t(exported.get("pages_decoded_on_worker_threads", int64_t(-1))) == int64_t(p_worker ? expected_pages : 0u));
	CHECK(int64_t(exported.get("pages_decoded_on_main_thread", int64_t(-1))) == int64_t(p_worker ? 0u : expected_pages));
	CHECK(int64_t(exported.get("page_failures", int64_t(-1))) == 0);
	CHECK(int64_t(exported.get("stored_bytes_read", int64_t(-1))) == int64_t(expected_bytes));
	CHECK(String(exported.get("last_failure_reason", "missing")).is_empty());
	CHECK(int64_t(exported.get("reads_submitted", int64_t(-1))) == int64_t(expected_pages));
	CHECK(int64_t(exported.get("reads_completed_ok", int64_t(-1))) == int64_t(expected_pages));
	CHECK(int64_t(exported.get("reads_completed_failed", int64_t(-1))) == 0);
	CHECK(int64_t(exported.get("reads_refused_busy", int64_t(-1))) == 0);
	CHECK(int64_t(exported.get("read_bytes", int64_t(-1))) == int64_t(expected_bytes));
	CHECK(int64_t(exported.get("read_handle_opens", int64_t(-1))) == int64_t(expected_pages));
	CHECK(bool(exported.get("decoded_digest_verified", !p_digest_checked)) == p_digest_checked);
	CHECK(int64_t(exported.get("decode_nsec", int64_t(-1))) == int64_t(stats.decode_nsec));
	CHECK(String(exported.get("digest_status", "")) == (p_digest_checked ? "checked" : "skipped"));
	if (p_digest_checked) {
		CHECK(exported.get("digest_nsec", Variant()).get_type() == Variant::INT);
		CHECK(int64_t(exported.get("digest_nsec", int64_t(-1))) == int64_t(stats.digest_nsec));
	} else {
		CHECK(exported.get("digest_nsec", int64_t(-1)).get_type() == Variant::NIL);
	}
	CHECK(int64_t(exported.get("verify_nsec", int64_t(-1))) == int64_t(stats.verify_nsec));
}

inline void identity(bool p_verify_digest) {
	Fixture fixture(p_verify_digest ? "identity_checked" : "identity_skipped");
	if (!fixture.valid) {
		FAIL("could not construct the independent plain-GSQ reference: ", std::string(fixture.error.utf8().get_data()));
		return;
	}
	for (bool worker : { false, true }) {
		INFO("worker capture: ", worker, "; decoded digest checked: ", p_verify_digest);
		CaptureRun run;
		run.source.instantiate();
		if (run.source.is_null()) {
			FAIL("could not allocate the compatibility adapter");
			return;
		}
		CHECK(run.source->is_decoded_digest_verified()); // source default remains enabled
		run.source->set_verify_decoded_digest(p_verify_digest);
		if (!run.source->configure(fixture.path, fixture.count, fixture.sh_degree, fixture.sh_first_order,
					fixture.sh_high_order, fixture.bounds, fixture.codec_params, fixture.pages)) {
			FAIL("the unchanged adapter refused the writer's valid immutable directory");
			return;
		}
		run.work = ranges(fixture);
		if (worker) {
			Thread thread;
			thread.start(&CaptureRun::run, &run);
			if (!thread.is_started()) {
				FAIL("could not start the CPU capture worker");
				return;
			}
			thread.wait_to_finish();
		} else {
			CaptureRun::run(&run);
		}
		CHECK(run.ran_on_main_thread == !worker);
		if (run.captures.size() != 11u || run.work.size() != 11u) {
			FAIL("the identity corpus did not execute all eleven captures");
			return;
		}
		for (uint32_t i = 0; i < run.captures.size(); i++) {
			INFO("capture ", i);
			if (!check_payload(fixture, run.work[i], run.captures[i])) {
				return;
			}
			if (i < 5u && run.captures[i].ok) {
				Digest actual;
				gs_gsq::decoded_digest(run.captures[i].gaussians.ptr(), run.captures[i].sh.ptr(),
						run.captures[i].gaussians.size(), fixture.sh_degree, actual.bytes);
				CHECK(memcmp(actual.bytes, fixture.reference_digests[i].bytes, gs_gsq::kDigestBytes) == 0);
				CHECK(memcmp(actual.bytes, fixture.pages[i].decoded_digest, gs_gsq::kDigestBytes) == 0);
			}
		}
		check_stats(fixture, *run.source.ptr(), p_verify_digest, worker);
		// A worker's per-thread decode sample cannot leak to the calling main thread.
		CHECK(run.source->take_thread_last_capture_decode_nsec() == 0u);
	}
}

} // namespace TestPayloadPageStoreIdentity

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStoreIdentity] chunk adapter preserves codec and packed payload digests with decoded verification enabled") {
	TestPayloadPageStoreIdentity::identity(true);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStoreIdentity] chunk adapter preserves codec and packed payload digests with decoded verification skipped") {
	TestPayloadPageStoreIdentity::identity(false);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStoreIdentity] staged streaming skips decoded digests while a source keeps its enabled default") {
	using namespace TestPayloadPageStore;
	Fixture fixture("identity_streaming_policy");
	if (!fixture.valid) {
		FAIL("could not construct the streaming-policy fixture: ", std::string(fixture.error.utf8().get_data()));
		return;
	}
	ResourceFormatLoaderGaussianSplatWorld loader;
	Error error = ERR_BUG;
	Ref<GaussianSplatWorld> world = loader.load(fixture.path, "", &error);
	if (error != OK || world.is_null() || world->get_chunk_payload_source().is_null()) {
		FAIL("the production streaming loader did not publish a chunk payload source");
		return;
	}
	GsqPagedChunkPayloadSource *source = Object::cast_to<GsqPagedChunkPayloadSource>(world->get_chunk_payload_source().ptr());
	if (source == nullptr) {
		FAIL("the production streaming loader published a different source type");
		return;
	}
	CHECK_FALSE(source->is_decoded_digest_verified());
	CHECK(bool(source->get_decode_stats().get("decoded_digest_verified", true)) == false);
	CHECK(source->get_decode_stats_struct().reads.submitted == 0u);
	CHECK(world->get_gaussian_data().is_null());
	Ref<GsqPagedChunkPayloadSource> default_source;
	default_source.instantiate();
	if (default_source.is_null()) {
		FAIL("could not allocate a default chunk source");
		return;
	}
	CHECK(default_source->is_decoded_digest_verified());
	CHECK(GsqPagedChunkPayloadSource::kReaderCount == 2u);
	CHECK(GsqPagedChunkPayloadSource::kMaxBytesInFlight == 64ull * 1024ull * 1024ull);
	CHECK(GsqPagedChunkPayloadSource::kMaxIdleHandles == 0u);
}
