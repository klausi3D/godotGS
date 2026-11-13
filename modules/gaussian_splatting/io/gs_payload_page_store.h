#pragma once

#include "gs_payload_read_service.h"
#include "gsq/gsq_codec.h"

#include "core/templates/pair.h"

// D3-1 transport/GSQ primitives. No directory interpretation, residency or GPU
// policy. Source installation/start/destruction belong to the owner; requests,
// waits, requester cancellation, leases, captures and stats are thread-safe.
class PayloadPageStore {
public:
	enum AdmissionPolicy {
		ADMIT_SERVICE_BYTES_ONLY,
		ADMIT_RETAINED_BYTES,
	};

	struct Config {
		uint32_t reader_count = 2;
		uint64_t max_bytes_in_flight = 64ull * 1024ull * 1024ull;
		uint32_t max_idle_handles = 0;
		// D3-1 chunk compatibility: only the read service limits admission.
		// Accounting always includes completed stored bytes in either mode.
		// Owners needing a bound over all stored owners must opt in explicitly.
		AdmissionPolicy admission_policy = ADMIT_SERVICE_BYTES_ONLY;
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
		PayloadPageStore *owner = nullptr;
		Identity identity;
		PageDescriptor descriptor;
		Error error = ERR_UNAVAILABLE;
		Vector<uint8_t> bytes;
		uint64_t reserved_bytes = 0;

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
		PayloadPageStore *owner = nullptr;
		gs_gsq::DecodeContext *decoder = nullptr;
		bool retain_on_return = true;

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
		uint64_t refused_retained_busy = 0; // Store-only refusals; not a read-service counter.
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
	// and drop idle handles before installing a new path/generation/catalog. Counters
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
	bool uses_retained_admission() const;

	// V4 uses transient leases so idle scratch cannot escape the streamer's
	// working-set reservation. Existing chunk callers retain their pool policy.
	Error lease_decoder(DecoderLease &r_lease, bool p_retain_on_return = true);
	// Whole-page decode, matching the current chunk source. Always verifies stored
	// bytes/header/descriptor. Digest policy is explicit per call (no V4 default).
	// On failure: no output, DIGEST_NOT_PERFORMED, a gsq_* reason, one page failure.
	Error decode_gsq_page(const StoredPage &p_page, DecoderLease &p_decoder, bool p_verify_decoded_digest,
			DecodedPage &r_page, String &r_reason);
	// Decode a complete GSQ page inside an adapter-owned opaque envelope. Checks
	// the entire GSQ stored digest and expected shape before writing caller-owned
	// buffers. No canonical decoded digest exists in this contract: the adapter
	// keeps its own decoded-validation policy. Stage counters use the same meanings
	// as decode_gsq_page; chunk's directory and digest checks remain unchanged.
	Error decode_gsq_subrange(const StoredPage &p_page, DecoderLease &p_decoder, uint32_t p_offset,
			uint32_t p_count, uint32_t p_sh_degree, Gaussian *r_gaussians, Vector3 *r_sh, String &r_reason);
	// Adapter capture boundary; page decode counts stay separate from captures.
	// Thread-local decode span is reset at begin and taken exactly once after end.
	// The adapter's legacy null-index refusal counts a capture without resetting it.
	void begin_capture(bool p_reset_decode_span = true);
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

private:
	using RequestKey = Pair<uint64_t, uint64_t>; // requester, local ticket
	enum State {
		NEW,
		RUNNING,
		STOPPING,
		STOPPED,
	};
	struct Request {
		uint64_t service_ticket = 0;
		Identity identity;
		PageDescriptor descriptor;
		RequestKey route;
		bool in_stash = false;
		Error error = ERR_UNAVAILABLE;
		Vector<uint8_t> bytes;
		uint64_t reserved_bytes = 0;
	};

	// The service owns its own mutex/readers. Lock order, when both are needed:
	// mutex -> read_service's mutex. Readers never call back into the store.
	PayloadReadService read_service;
	// Guards ALL fields below, including lifecycle, catalog, dispatch/stash,
	// reservation counters, decoder pool, stage counters and test faults. Codec
	// work runs outside it, using caller-owned StoredPage/DecoderLease instances.
	// Those move-only instances must not themselves be shared concurrently.
	mutable BinaryMutex mutex;
	ConditionVariable dispatch_cv;
	State state = NEW;
	Config config;
	String file_path;
	uint64_t source_generation = 0;
	HashMap<uint64_t, PageDescriptor> catalog;
	uint64_t next_service_ticket = 0;
	HashMap<RequestKey, uint64_t> request_keys;
	HashMap<uint64_t, Request *> requests;
	HashMap<RequestKey, Request *> dispatch_stash;
	bool dispatch_pumping = false;
	uint32_t waiters = 0;
	LocalVector<gs_gsq::DecodeContext *> idle_decoders;
	Stats counters;
#ifdef TESTS_ENABLED
	TestFault test_fault = TEST_FAULT_NONE;
	uint64_t test_fault_count = 0;
	uint64_t test_first_ticket = 0;
	uint64_t test_second_ticket = 0;
#endif

	static bool _descriptors_equal(const PageDescriptor &p_a, const PageDescriptor &p_b);
	bool _pump_locked(const MutexLock<BinaryMutex> &p_lock, uint64_t p_watch_ticket);
	void _route_locked(PayloadReadService::Completion &p_completion);
	void _take_stash_locked(const RequestKey &p_key, StoredPage &r_page);
	void _discard_locked(Request *p_request);
	void _drain_locked();
	void _release_stored(uint64_t p_bytes);
	void _return_decoder(gs_gsq::DecodeContext *p_decoder, bool p_retain);
};
