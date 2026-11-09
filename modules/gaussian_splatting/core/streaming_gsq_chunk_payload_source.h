#ifndef STREAMING_GSQ_CHUNK_PAYLOAD_SOURCE_H
#define STREAMING_GSQ_CHUNK_PAYLOAD_SOURCE_H

#include "../io/gs_payload_read_service.h"
#include "../io/gsq/gsq_codec.h"
#include "streaming_chunk_payload_source.h"

#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

#include <cstdint>

// Codec-aware staged source for plain v3 + bit 7 worlds, whose chunks are runs of GSQ v1 pages
// (BV-C5b; docs/architecture/adr-beyond-vram-worlds.md D-12 "Decode location", §3.1, §3.4 step 5).
//
// The world loader validates the page directory and hands it over; no page is read at load. A
// capture runs on the caller's thread, which on the streaming route is an async pack worker
// (StreamingUploadPipeline::build_pending_upload_from_pack_job): it reads every page the range
// touches through this source's PayloadReadService (positional reads on the reader pool), checks
// each page against its directory entry and stored SHA-256, decodes it with a per-thread
// gs_gsq::DecodeContext (one ZSTD_DCtx each, leased from a pool) and checks the decoded digest.
// The pack worker then packs the result into the 176 B PackedGaussian, as for any other source.
//
// Failure is visible, never repaired or skipped: a capture that meets a short read, a digest or
// table mismatch or a refused decode returns false (the streaming system counts it in
// chunk_payload_read_failures and retries with backoff), increments page_failures and records
// the gsq_* reason in last_failure_reason.
//
// Telemetry (release counters, get_decode_stats()): pages and splats decoded, decode time, and
// where each page was decoded (worker thread or main thread), so a test or probe can assert that
// the async path ran instead of a main-thread or resident fallback.
//
// Threading: configure() runs once before publication. Captures may run concurrently from any
// thread. Completions of the shared read service are routed to their capture by ticket through
// `dispatch_mutex` (one thread at a time waits on the service and parks foreign completions in
// `dispatch_stash`).
class GsqPagedChunkPayloadSource : public ChunkPayloadSource {
	GDCLASS(GsqPagedChunkPayloadSource, ChunkPayloadSource);

public:
	// One page of the directory, as validated by the loader (gaussian_splat_world_format.h).
	struct Page {
		uint64_t offset = 0;
		uint32_t stored_bytes = 0;
		uint32_t splat_count = 0;
		uint8_t stored_digest[gs_gsq::kDigestBytes] = {};
		uint8_t decoded_digest[gs_gsq::kDigestBytes] = {};
		uint32_t extent[6] = {}; // float bit patterns: position min xyz, max xyz
	};

	struct DecodeStats {
		uint64_t captures = 0;
		uint64_t captures_failed = 0;
		uint64_t pages_decoded = 0;
		uint64_t splats_decoded = 0;
		uint64_t decode_nsec = 0; // verify + decode + decoded digest, summed over pages
		uint64_t digest_nsec = 0; // the decoded-digest share of decode_nsec (BV-K9)
		uint64_t pages_decoded_on_worker_threads = 0;
		uint64_t pages_decoded_on_main_thread = 0;
		uint64_t page_failures = 0;
		uint64_t stored_bytes_read = 0;
		String last_failure_reason;
		PayloadReadService::Stats reads;
	};

	// Reader pool of the source's PayloadReadService (started on the first capture).
	static constexpr uint32_t kReaderCount = 2;
	static constexpr uint64_t kMaxBytesInFlight = 64ull * 1024ull * 1024ull;
	// No idle handles: a reader closes its handle after each page read, so no handle stays open
	// on the world file between captures. On Windows an open handle denies replacing the file,
	// and ScopedReaderSuspend (#714) does not know this source, so a cached handle would make a
	// reimport of a streamed world fail. A read in flight during the rename still can (follow-up).
	static constexpr uint32_t kMaxIdleHandles = 0;

protected:
	static void _bind_methods();

private:
	String file_path;
	uint32_t splat_count = 0;
	uint32_t sh_degree = 0;
	uint32_t sh_first_order = 0;
	uint32_t sh_high_order = 0;
	AABB bounds;
	gs_gsq::EncodeParams codec_params;
	LocalVector<Page> pages;
	LocalVector<uint32_t> page_first_splat; // page_count + 1 entries; the last is splat_count

	mutable PayloadReadService read_service;
	mutable BinaryMutex state_mutex; // guards the fields below up to dispatch_mutex
	mutable bool service_started = false;
	mutable bool service_failed = false;
	mutable LocalVector<gs_gsq::DecodeContext *> idle_decoders;
	mutable String last_failure_reason;

	mutable bool failure_logged = false;

	mutable BinaryMutex dispatch_mutex; // guards the fields below
	mutable ConditionVariable dispatch_cv;
	mutable bool dispatch_pumping = false;
	mutable HashMap<uint64_t, PayloadReadService::Completion> dispatch_stash;

	mutable SafeNumeric<uint64_t> next_ticket;
	mutable SafeNumeric<uint64_t> stat_captures;
	mutable SafeNumeric<uint64_t> stat_captures_failed;
	mutable SafeNumeric<uint64_t> stat_pages_decoded;
	mutable SafeNumeric<uint64_t> stat_splats_decoded;
	mutable SafeNumeric<uint64_t> stat_decode_nsec;
	mutable SafeNumeric<uint64_t> stat_digest_nsec;
	mutable SafeNumeric<uint64_t> stat_pages_on_workers;
	mutable SafeNumeric<uint64_t> stat_pages_on_main;
	mutable SafeNumeric<uint64_t> stat_page_failures;
	mutable SafeNumeric<uint64_t> stat_stored_bytes_read;

	bool _ensure_service_started() const;
	uint32_t _page_of(uint32_t p_splat) const;
	// Waits for p_ticket's completion. False when the read service stopped first.
	bool _dispatch_wait(uint64_t p_ticket, PayloadReadService::Completion *r_completion) const;
	gs_gsq::DecodeContext *_lease_decoder() const;
	void _return_decoder(gs_gsq::DecodeContext *p_decoder) const;
	bool _fail(const String &p_reason) const;
	bool _capture(const uint32_t *p_indices, uint32_t p_start, uint32_t p_count,
			LocalVector<Gaussian> &r_gaussians, LocalVector<Vector3> &r_sh_high_order,
			uint32_t &r_sh_first_order_count, uint32_t &r_sh_high_order_count) const;

public:
	GsqPagedChunkPayloadSource() = default;
	~GsqPagedChunkPayloadSource() override;

	// Installs the validated directory. Pages tile [0, p_splat_count) in order. False (and
	// nothing installed) when the directory is empty or does not add up.
	bool configure(const String &p_path, uint32_t p_splat_count, uint32_t p_sh_degree, uint32_t p_sh_first_order,
			uint32_t p_sh_high_order, const AABB &p_bounds, const gs_gsq::EncodeParams &p_codec_params,
			const LocalVector<Page> &p_pages);

	bool capture_chunk_snapshot(uint32_t p_start, uint32_t p_count,
			LocalVector<Gaussian> &r_gaussians,
			LocalVector<Vector3> &r_sh_high_order,
			uint32_t &r_sh_first_order_count,
			uint32_t &r_sh_high_order_count) const override;

	bool capture_indexed_chunk_snapshot(const uint32_t *p_indices, uint32_t p_count,
			LocalVector<Gaussian> &r_gaussians,
			LocalVector<Vector3> &r_sh_high_order,
			uint32_t &r_sh_first_order_count,
			uint32_t &r_sh_high_order_count) const override;

	uint32_t get_count() const override { return splat_count; }
	uint32_t get_sh_degree() const override { return sh_degree; }
	AABB get_bounds() const override { return bounds; }
	bool is_valid() const override { return !file_path.is_empty() && splat_count > 0 && !pages.is_empty(); }
	uint64_t take_thread_last_capture_decode_nsec() const override;

	const String &get_file_path() const { return file_path; }
	uint32_t get_page_count() const { return pages.size(); }
	DecodeStats get_decode_stats_struct() const;
	Dictionary get_decode_stats() const;
};

#endif // STREAMING_GSQ_CHUNK_PAYLOAD_SOURCE_H
