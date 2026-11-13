#pragma once

#include "gs_payload_read_service.h"
#include "gsq/gsq_codec.h"

// D3-1 transport/GSQ primitives. Phase 1 declares the contract only; the .cpp is
// deliberately fail-closed. No directory interpretation, residency or GPU policy.
class PayloadPageStore {
public:
	struct Config {
		uint32_t reader_count = 2;
		uint64_t max_bytes_in_flight = 64ull * 1024ull * 1024ull;
		uint32_t max_idle_handles = 0;
		// The chunk source currently retains its peak concurrent decoder count,
		// without a fixed cap. UINT32_MAX preserves that behavior. Other owners may
		// supply their existing cap; this is not a new runtime sizing setting.
		uint32_t decoder_retention_cap = UINT32_MAX;
	};

	enum Encoding {
		OPAQUE_BYTES,
		GSQ_V1,
	};

	// Copied at source installation and thereafter immutable. IDs are opaque;
	// the adapter supplies the directory semantics. A request must match the
	// installed descriptor exactly, including ID, offset, size and codec fields.
	struct PageDescriptor {
		uint64_t id = 0;
		uint64_t offset = 0;
		uint32_t stored_bytes = 0;
		Encoding encoding = OPAQUE_BYTES;
		uint32_t splat_count = 0;
		uint32_t sh_degree = 0;
		gs_gsq::EncodeParams codec_params;
		uint8_t stored_digest[gs_gsq::kDigestBytes] = {};
		uint8_t decoded_digest[gs_gsq::kDigestBytes] = {};
		uint32_t extent[6] = {};
	};

	struct Identity {
		uint64_t source_generation = 0;
		uint64_t requester = 0;
		uint64_t ticket = 0;
		uint64_t page_id = 0;
	};

	// Move-only completion and its stored-byte reservation. Service -> stash ->
	// caller transfers retain the SAME reservation until reset/destruction. A
	// failed read carries its identity/error but no bytes. Store lifetime covers
	// all callers, stored pages and decoder leases, as for the existing source.
	class StoredPage {
		friend class PayloadPageStore;
		Vector<uint8_t> empty_bytes;

	public:
		StoredPage() = default;
		~StoredPage();
		StoredPage(const StoredPage &) = delete;
		StoredPage &operator=(const StoredPage &) = delete;
		StoredPage(StoredPage &&p_other) noexcept;
		StoredPage &operator=(StoredPage &&p_other) noexcept;
		bool is_valid() const;
		Identity get_identity() const;
		Error get_error() const;
		const Vector<uint8_t> &get_bytes() const;
		void reset(); // Idempotent; frees storage before releasing its reservation.
	};

	class DecoderLease {
		friend class PayloadPageStore;

	public:
		DecoderLease() = default;
		~DecoderLease();
		DecoderLease(const DecoderLease &) = delete;
		DecoderLease &operator=(const DecoderLease &) = delete;
		DecoderLease(DecoderLease &&p_other) noexcept;
		DecoderLease &operator=(DecoderLease &&p_other) noexcept;
		gs_gsq::DecodeContext *get() const; // Exclusive until reset/destruction.
		void reset();
	};

	enum DigestStatus {
		DIGEST_NOT_PERFORMED,
		DIGEST_CHECKED,
		DIGEST_SKIPPED,
	};

	struct DecodedPage {
		LocalVector<Gaussian> gaussians;
		LocalVector<Vector3> sh_high_order;
		DigestStatus digest_status = DIGEST_NOT_PERFORMED;
	};

	struct Stats {
		PayloadReadService::Stats reads;
		// Current holders, with ready service completions included in service_bytes.
		uint64_t service_bytes = 0;
		uint64_t stash_bytes = 0;
		uint64_t caller_bytes = 0;
		uint64_t retained_bytes = 0; // Sum of the three holders, not service alone.
		uint64_t peak_service_bytes = 0;
		uint64_t peak_stash_bytes = 0;
		uint64_t peak_retained_bytes = 0;
		uint64_t reservations_acquired = 0;
		uint64_t reservations_released = 0;
		uint64_t live_reservations = 0;
		uint64_t decoder_leases = 0;
		uint64_t decoder_returns = 0;
		uint64_t decoder_created = 0;
		uint64_t decoder_destroyed = 0;
		uint64_t decoder_reuses = 0;
		uint32_t decoders_leased = 0;
		uint32_t decoders_idle = 0;
		uint32_t peak_decoders_leased = 0;
		// Existing K9/K9b meanings: stage times and page/splat counts include
		// successful pages only; decode_nsec INCLUDES verify and decoded digest.
		// Failed pages add stored bytes, page_failures, and the capture failure,
		// but do not add successful-page stage times/counts. Not zstd timing.
		uint64_t captures = 0;
		uint64_t captures_failed = 0;
		uint64_t pages_verified = 0; // Successful decoded pages with stored verification.
		uint64_t pages_decoded = 0;
		uint64_t splats_decoded = 0;
		uint64_t decode_nsec = 0;
		uint64_t verify_nsec = 0;
		uint64_t digest_nsec = 0;
		uint64_t pages_decoded_digest_checked = 0;
		uint64_t pages_decoded_digest_skipped = 0;
		uint64_t pages_decoded_on_worker_threads = 0;
		uint64_t pages_decoded_on_main_thread = 0;
		uint64_t stored_bytes_read = 0;
		uint64_t page_failures = 0;
		String last_failure_reason;
	};

	PayloadPageStore() = default;
	~PayloadPageStore();
	PayloadPageStore(const PayloadPageStore &) = delete;
	PayloadPageStore &operator=(const PayloadPageStore &) = delete;

	Error start(const Config &p_config);
	// Owner operation, outside concurrent calls: drain old service/stash owners
	// and close handles before installing a new path/generation/catalog. Counters
	// are cumulative across replacement. Caller-held pages/leases must be returned.
	Error replace_source(const String &p_path, uint64_t p_generation, const LocalVector<PageDescriptor> &p_pages);
	// Ticket uniqueness is (requester, ticket) within a source generation, so two
	// requesters may use the same local ticket without crossing completions.
	PayloadReadService::SubmitResult request(uint64_t p_requester, uint64_t p_ticket, const PageDescriptor &p_page);
	// Blocks using read-service completion primitives, routing foreign results to
	// the stash. False (empty output) for unknown/cancelled/stale/stopped tickets.
	bool wait(uint64_t p_requester, uint64_t p_ticket, StoredPage &r_page);
	// Synchronous cancellation/drain of service AND stash ownership for requester.
	// Idempotent. Wakes its waiters. Does not cancel other requesters.
	Error cancel_requester(uint64_t p_requester);
	void shutdown(); // Idempotent; joins readers and drains stored/idle decoder owners.
	Stats stats() const;

	Error lease_decoder(DecoderLease &r_lease);
	// Whole-page decode, matching the current chunk source. Always verifies stored
	// bytes/header/descriptor. Digest policy is explicit per call (no V4 default).
	// On failure: no output, DIGEST_NOT_PERFORMED, a gsq_* reason, one page failure.
	Error decode_gsq_page(const StoredPage &p_page, DecoderLease &p_decoder, bool p_verify_decoded_digest,
			DecodedPage &r_page, String &r_reason);
	// Adapter capture boundary; page decode counts stay separate from captures.
	// Thread-local decode span is reset at begin and taken exactly once after end.
	void begin_capture();
	void end_capture(bool p_success, const String &p_reason);
	uint64_t take_thread_last_capture_decode_nsec();

#ifdef TESTS_ENABLED
	// Same compile gate as PayloadReadService::_test_set_read_hook: absent from
	// non-test release templates. Faults alter the REAL dispatch handoff, never
	// stats() or the checker. Set only while drained, before submitting requests.
	enum TestFault {
		TEST_FAULT_NONE,
		// Dispatch still parks real bytes, but omits their reservation transfer.
		TEST_FAULT_DROP_STASH_TRANSFER,
		// Exchange completion routing keys (requester AND ticket) of the first
		// two accepted requests from different requesters, leaving bytes intact.
		TEST_FAULT_SWAP_REQUESTER_TICKETS,
	};
	Error _test_set_fault(TestFault p_fault);
	// Waits for one real service completion and dispatches it into the stash.
	// Only called without concurrent wait() pumps; enables deterministic snapshots.
	bool _test_dispatch_one_completion();
	uint64_t _test_faults_triggered() const;
#endif
};
