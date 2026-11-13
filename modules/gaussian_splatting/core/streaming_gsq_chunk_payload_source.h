#ifndef STREAMING_GSQ_CHUNK_PAYLOAD_SOURCE_H
#define STREAMING_GSQ_CHUNK_PAYLOAD_SOURCE_H

#include "../io/gs_payload_page_store.h"
#include "streaming_chunk_payload_source.h"

#include "core/os/mutex.h"
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
// touches through its PayloadPageStore (positional reads on the reader pool), checks
// each page against its directory entry and stored SHA-256, decodes it with a per-thread
// gs_gsq::DecodeContext (one ZSTD_DCtx each, leased from a pool) and, when enabled, checks the
// decoded digest. The pack worker then packs the result into the 176 B PackedGaussian, as for
// any other source.
//
// Decoded digest (BV-K9c): a source checks it by default (set_verify_decoded_digest). The
// streaming load turns it off (gaussian_splat_world_io.cpp, staged branch): measured on the
// tier-A flythrough it was 68.5 % of the per-page decode span (run 42687229044). It is redundant
// there for on-disk integrity: verify_page() checks the header and the stored SHA-256 over every
// stored byte, the page must match its page-table entry, and zstd checks each frame's checksum.
// The decoded digest only re-hashes the output of a deterministic decode of those verified bytes,
// so on this path it can catch only a decoder defect, which the codec golden-digest doctests and
// the resident load (load_resident(), which still checks it) cover. The stored-page verify is
// never skipped. Whether the check ran is exported (decoded_digest_verified,
// pages_decoded_digest_checked), so a zero digest_nsec reads as "skipped", not as "free".
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
// thread. The store owns the reader service, completion routing, decoder pool and counters.
// `state_mutex` serializes lazy store startup and the once-per-source failure log.
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
		uint64_t digest_nsec = 0; // the decoded-digest share of decode_nsec (BV-K9); 0 when not verified
		uint64_t pages_decoded_digest_checked = 0; // pages whose decoded digest was checked (BV-K9c)
		uint64_t verify_nsec = 0; // the stored-page verify share (header + stored SHA-256) of decode_nsec
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
	LocalVector<PayloadPageStore::PageDescriptor> pages;
	LocalVector<uint32_t> page_first_splat; // page_count + 1 entries; the last is splat_count
	// Set before the source is published (like configure()); captures only read it.
	bool verify_decoded_digest = true;

	mutable PayloadPageStore page_store;
	mutable BinaryMutex state_mutex; // guards startup flags and failure_logged
	mutable bool service_started = false;
	mutable bool service_failed = false;
	mutable bool failure_logged = false;
	mutable SafeNumeric<uint64_t> next_requester;

	bool _ensure_service_started() const;
	uint32_t _page_of(uint32_t p_splat) const;
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

	// Whether a capture re-hashes each decoded page and compares it with the page-table decoded
	// digest. Default true; the streaming load sets false (BV-K9c, see the class comment). Call
	// before the source is published to a streaming system or captured from.
	void set_verify_decoded_digest(bool p_verify) { verify_decoded_digest = p_verify; }
	bool is_decoded_digest_verified() const { return verify_decoded_digest; }

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
