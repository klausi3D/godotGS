#include "gs_payload_page_store.h"

#include "core/os/thread.h"
#include "core/os/os.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace {
// K9's per-thread, take-once span is shared across source instances on a thread,
// matching the original chunk source. It is not worker wall latency or zstd time.
thread_local uint64_t last_capture_decode_nsec = 0;
} // namespace

PayloadPageStore::~PayloadPageStore() {
	shutdown();
}

PayloadPageStore::StoredPage::~StoredPage() {
	reset();
}

PayloadPageStore::StoredPage::StoredPage(StoredPage &&p_other) noexcept {
	*this = std::move(p_other);
}

PayloadPageStore::StoredPage &PayloadPageStore::StoredPage::operator=(StoredPage &&p_other) noexcept {
	if (this != &p_other) {
		reset();
		owner = p_other.owner;
		identity = p_other.identity;
		descriptor = p_other.descriptor;
		error = p_other.error;
		bytes = std::move(p_other.bytes);
		reserved_bytes = p_other.reserved_bytes;
		p_other.owner = nullptr;
		p_other.identity = Identity();
		p_other.error = ERR_UNAVAILABLE;
		p_other.reserved_bytes = 0;
	}
	return *this;
}

bool PayloadPageStore::StoredPage::is_valid() const {
	return owner != nullptr;
}

PayloadPageStore::Identity PayloadPageStore::StoredPage::get_identity() const {
	return identity;
}

Error PayloadPageStore::StoredPage::get_error() const {
	return error;
}

const Vector<uint8_t> &PayloadPageStore::StoredPage::get_bytes() const {
	return bytes;
}

void PayloadPageStore::StoredPage::reset() {
	bytes.clear(); // Free storage before releasing its reservation.
	if (owner != nullptr) {
		PayloadPageStore *store = owner;
		owner = nullptr;
		store->_release_stored(reserved_bytes);
	}
	reserved_bytes = 0;
	identity = Identity();
	error = ERR_UNAVAILABLE;
}

PayloadPageStore::DecoderLease::~DecoderLease() {
	reset();
}

PayloadPageStore::DecoderLease::DecoderLease(DecoderLease &&p_other) noexcept {
	*this = std::move(p_other);
}

PayloadPageStore::DecoderLease &PayloadPageStore::DecoderLease::operator=(DecoderLease &&p_other) noexcept {
	if (this != &p_other) {
		reset();
		owner = p_other.owner;
		decoder = p_other.decoder;
		retain_on_return = p_other.retain_on_return;
		p_other.owner = nullptr;
		p_other.decoder = nullptr;
	}
	return *this;
}

gs_gsq::DecodeContext *PayloadPageStore::DecoderLease::get() const {
	return decoder;
}

void PayloadPageStore::DecoderLease::reset() {
	if (owner != nullptr) {
		PayloadPageStore *store = owner;
		gs_gsq::DecodeContext *returned = decoder;
		owner = nullptr;
		decoder = nullptr;
		store->_return_decoder(returned, retain_on_return);
	}
}

bool PayloadPageStore::_descriptors_equal(const PageDescriptor &p_a, const PageDescriptor &p_b) {
	// Compare fields, never padding or floats under fast-math equality.
	return p_a.id == p_b.id && p_a.offset == p_b.offset && p_a.stored_bytes == p_b.stored_bytes &&
			p_a.encoding == p_b.encoding && p_a.splat_count == p_b.splat_count && p_a.sh_degree == p_b.sh_degree &&
			memcmp(p_a.codec_params.sh_bits, p_b.codec_params.sh_bits, sizeof(p_a.codec_params.sh_bits)) == 0 &&
			p_a.codec_params.opacity_bits == p_b.codec_params.opacity_bits &&
			memcmp(&p_a.codec_params.pos_step_m, &p_b.codec_params.pos_step_m, sizeof(float)) == 0 &&
			p_a.codec_params.zstd_level == p_b.codec_params.zstd_level &&
			memcmp(p_a.stored_digest, p_b.stored_digest, sizeof(p_a.stored_digest)) == 0 &&
			memcmp(p_a.decoded_digest, p_b.decoded_digest, sizeof(p_a.decoded_digest)) == 0 &&
			memcmp(p_a.extent, p_b.extent, sizeof(p_a.extent)) == 0;
}

Error PayloadPageStore::start(const Config &p_config) {
	MutexLock lock(mutex);
	if (state != NEW) {
		return ERR_ALREADY_IN_USE;
	}
	if (p_config.reader_count == 0 || p_config.max_bytes_in_flight == 0 ||
			(p_config.admission_policy != ADMIT_SERVICE_BYTES_ONLY && p_config.admission_policy != ADMIT_RETAINED_BYTES)) {
		return ERR_INVALID_PARAMETER;
	}
	config = p_config;
	PayloadReadService::Config read_config;
	read_config.reader_count = config.reader_count;
	read_config.max_bytes_in_flight = config.max_bytes_in_flight;
	read_config.max_idle_handles = config.max_idle_handles;
	const Error error = read_service.start(read_config);
	state = error == OK ? RUNNING : STOPPED;
	// Lazy adapters begin a capture before start(), including empty/invalid
	// captures. Neither startup nor source installation resets counters or TLS.
	return error;
}

Error PayloadPageStore::replace_source(const String &p_path, uint64_t p_generation, const LocalVector<PageDescriptor> &p_pages,
		uint64_t p_collection_epoch) {
	if (p_path.is_empty() || p_generation == 0 || p_pages.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	HashMap<uint64_t, PageDescriptor> replacement;
	for (const PageDescriptor &page : p_pages) {
		if (replacement.has(page.id) || page.stored_bytes == 0 || page.offset > uint64_t(INT64_MAX) ||
				page.stored_bytes > uint64_t(INT64_MAX) - page.offset ||
				(page.encoding != OPAQUE_BYTES && page.encoding != GSQ_V1)) {
			return ERR_INVALID_PARAMETER;
		}
		if (page.encoding == GSQ_V1 && (page.stored_bytes < gs_gsq::kPageHeaderBytes || page.splat_count == 0 ||
				page.splat_count > gs_gsq::kMaxPageSplats || page.sh_degree > gs_gsq::kMaxShDegree)) {
			return ERR_INVALID_PARAMETER;
		}
		replacement.insert(page.id, page);
	}
	MutexLock lock(mutex);
	if (state != RUNNING) {
		return ERR_UNAVAILABLE;
	}
	if (counters.live_reservations != requests.size() || counters.decoders_leased != 0 || dispatch_pumping || waiters != 0) {
		return ERR_BUSY; // Active callers must return pages/leases first.
	}
	if (source_generation != 0 && p_generation <= source_generation) {
		return ERR_INVALID_PARAMETER;
	}
	_drain_locked();
	read_service.release_handles();
	read_service.reset_observation_peaks();
	if (source_generation != 0) {
		collection_base = counters;
		collection_base.reads = read_service.stats();
	}
	// On FIRST installation, a lazy chunk capture has already begun. Include it.
	counters.source_generation = p_generation;
	counters.collection_epoch = p_collection_epoch != 0 ? p_collection_epoch : p_generation;
	file_path = p_path;
	source_generation = p_generation;
	catalog = replacement;
	dispatch_cv.notify_all();
	return OK;
}

PayloadReadService::SubmitResult PayloadPageStore::request(uint64_t p_requester, uint64_t p_ticket, const PageDescriptor &p_page) {
	MutexLock lock(mutex);
	if (state != RUNNING || file_path.is_empty()) {
		return PayloadReadService::SUBMIT_NOT_RUNNING;
	}
	const PageDescriptor *installed = catalog.getptr(p_page.id);
	if (installed == nullptr || !_descriptors_equal(*installed, p_page)) {
		return PayloadReadService::SUBMIT_INVALID;
	}
	const RequestKey key(p_requester, p_ticket);
	if (request_keys.has(key)) {
		return PayloadReadService::SUBMIT_DUPLICATE_TICKET;
	}
	if (p_page.stored_bytes > config.max_bytes_in_flight) {
		return PayloadReadService::SUBMIT_TOO_LARGE;
	}
	if (next_service_ticket == UINT64_MAX) {
		return PayloadReadService::SUBMIT_INVALID;
	}
	if (config.admission_policy == ADMIT_RETAINED_BYTES &&
			(counters.retained_bytes > config.max_bytes_in_flight ||
					p_page.stored_bytes > config.max_bytes_in_flight - counters.retained_bytes)) {
		counters.refused_retained_busy++;
		return PayloadReadService::SUBMIT_BUSY;
	}
	const uint64_t service_ticket = ++next_service_ticket;
	// Qualified name: file I/O, not RenderingDevice submission (#685).
	const PayloadReadService::SubmitResult result = read_service.PayloadReadService::submit(
			file_path, p_page.offset, p_page.stored_bytes, service_ticket);
	if (result != PayloadReadService::SUBMIT_OK) {
		return result;
	}
	Request *request = memnew(Request);
	request->service_ticket = service_ticket;
	request->identity.source_generation = source_generation;
	request->identity.requester = p_requester;
	request->identity.ticket = p_ticket;
	request->identity.page_id = p_page.id;
	request->descriptor = *installed;
	request->route = key;
	request->reserved_bytes = p_page.stored_bytes;
	request_keys.insert(key, service_ticket);
	requests.insert(service_ticket, request);
	counters.service_bytes += request->reserved_bytes;
	counters.retained_bytes += request->reserved_bytes;
	counters.peak_service_bytes = MAX(counters.peak_service_bytes, counters.service_bytes);
	counters.peak_retained_bytes = MAX(counters.peak_retained_bytes, counters.retained_bytes);
	counters.reservations_acquired++;
	counters.live_reservations++;
#ifdef TESTS_ENABLED
	if (test_fault == TEST_FAULT_SWAP_REQUESTER_TICKETS && test_second_ticket == 0) {
		Request *const *first = requests.getptr(test_first_ticket);
		if (first == nullptr) {
			test_first_ticket = service_ticket;
		} else if ((*first)->identity.requester != p_requester) {
			test_second_ticket = service_ticket;
		}
	}
#endif
	return PayloadReadService::SUBMIT_OK;
}

bool PayloadPageStore::_pump_locked(const MutexLock<BinaryMutex> &p_lock, uint64_t p_watch_ticket) {
	dispatch_pumping = true;
	p_lock.temp_unlock();
	// Wait WITHOUT taking bytes. Actual take and reservation transfer occur
	// together under the store mutex: neither stats nor admission sees a gap.
	const bool ready = read_service.wait_ready(p_watch_ticket);
	p_lock.temp_relock();
	bool received = false;
	if (ready && state == RUNNING) {
		PayloadReadService::Completion completion;
		received = read_service.poll_completion(completion);
		if (received) {
			_route_locked(completion);
		}
	}
	dispatch_pumping = false;
	dispatch_cv.notify_all();
	return received;
}

void PayloadPageStore::_route_locked(PayloadReadService::Completion &p_completion) {
	Request **found = requests.getptr(p_completion.ticket);
	if (found == nullptr) {
		return; // Cancellation must never resurrect a result.
	}
	Request *request = *found;
	request->in_stash = true;
	request->error = p_completion.error;
	request->bytes = std::move(p_completion.bytes);
	counters.service_bytes -= request->reserved_bytes;
#ifdef TESTS_ENABLED
	if (test_fault == TEST_FAULT_DROP_STASH_TRANSFER) {
		// Park the real bytes but omit their reservation transfer. The live
		// owner count survives so fault cleanup cannot underflow byte counters.
		counters.retained_bytes -= request->reserved_bytes;
		request->reserved_bytes = 0;
		test_fault_count++;
	}
	if (test_fault == TEST_FAULT_SWAP_REQUESTER_TICKETS && test_first_ticket != 0 && test_second_ticket != 0) {
		uint64_t other_ticket = 0;
		if (p_completion.ticket == test_first_ticket) {
			other_ticket = test_second_ticket;
		} else if (p_completion.ticket == test_second_ticket) {
			other_ticket = test_first_ticket;
		}
		Request *const *other = requests.getptr(other_ticket);
		if (other != nullptr) {
			request->route = RequestKey((*other)->identity.requester, (*other)->identity.ticket);
			test_fault_count++;
		}
	}
#endif
	counters.stash_bytes += request->reserved_bytes;
	counters.peak_stash_bytes = MAX(counters.peak_stash_bytes, counters.stash_bytes);
	dispatch_stash.insert(request->route, request);
}

void PayloadPageStore::_take_stash_locked(const RequestKey &p_key, StoredPage &r_page) {
	Request *request = dispatch_stash[p_key];
	dispatch_stash.erase(p_key);
	request_keys.erase(RequestKey(request->identity.requester, request->identity.ticket));
	requests.erase(request->service_ticket);
	counters.stash_bytes -= request->reserved_bytes;
	counters.caller_bytes += request->reserved_bytes;
	r_page.owner = this;
	r_page.identity = request->identity;
	r_page.descriptor = request->descriptor;
	r_page.error = request->error;
	r_page.bytes = std::move(request->bytes);
	r_page.reserved_bytes = request->reserved_bytes;
	memdelete(request);
}

bool PayloadPageStore::wait(uint64_t p_requester, uint64_t p_ticket, StoredPage &r_page) {
	r_page.reset();
	MutexLock lock(mutex);
	const RequestKey key(p_requester, p_ticket);
	uint64_t expected_ticket = 0;
	if (Request *const *parked = dispatch_stash.getptr(key)) {
		expected_ticket = (*parked)->service_ticket;
	} else if (const uint64_t *submitted = request_keys.getptr(key)) {
		expected_ticket = *submitted;
	}
	if (state != RUNNING || expected_ticket == 0) {
		return false;
	}
	waiters++;
	bool taken = false;
	// Pin the internal ticket: cancellation followed by local-ticket reuse
	// cannot turn an old wait into a wait for the new request.
	while (state == RUNNING && requests.has(expected_ticket)) {
		if (dispatch_stash.has(key)) {
			_take_stash_locked(key, r_page);
			taken = true;
			break;
		}
		if (dispatch_pumping) {
			dispatch_cv.wait(lock);
		} else {
			_pump_locked(lock, expected_ticket);
		}
	}
	waiters--;
	dispatch_cv.notify_all();
	return taken;
}

void PayloadPageStore::_discard_locked(Request *p_request) {
	if (p_request->in_stash) {
		dispatch_stash.erase(p_request->route);
		p_request->bytes.clear();
		counters.stash_bytes -= p_request->reserved_bytes;
	} else {
		// The caller has already cancelled/drained ALL affected service tickets
		// together, so one slow reader cannot delay cancellation of its siblings.
		counters.service_bytes -= p_request->reserved_bytes;
	}
	counters.retained_bytes -= p_request->reserved_bytes;
	counters.reservations_released++;
	counters.live_reservations--;
	request_keys.erase(RequestKey(p_request->identity.requester, p_request->identity.ticket));
	requests.erase(p_request->service_ticket);
	memdelete(p_request);
}

void PayloadPageStore::_drain_locked() {
	// HashMap erasure invalidates iterators; collect IDs first.
	LocalVector<uint64_t> tickets;
	for (const KeyValue<uint64_t, Request *> &entry : requests) {
		tickets.push_back(entry.key);
	}
	read_service.cancel_and_wait(tickets);
	for (uint64_t ticket : tickets) {
		_discard_locked(requests[ticket]);
	}
}

Error PayloadPageStore::cancel_requester(uint64_t p_requester) {
	MutexLock lock(mutex);
	LocalVector<uint64_t> tickets;
	for (const KeyValue<uint64_t, Request *> &entry : requests) {
		if (entry.value->identity.requester == p_requester) {
			tickets.push_back(entry.key);
		}
	}
	read_service.cancel_and_wait(tickets);
	for (uint64_t ticket : tickets) {
		_discard_locked(requests[ticket]);
	}
	dispatch_cv.notify_all();
	return OK;
}

void PayloadPageStore::_release_stored(uint64_t p_bytes) {
	MutexLock lock(mutex);
	counters.caller_bytes -= p_bytes;
	counters.retained_bytes -= p_bytes;
	counters.live_reservations--;
	counters.reservations_released++;
	dispatch_cv.notify_all();
}

void PayloadPageStore::shutdown() {
	MutexLock lock(mutex);
	while (state == STOPPING) {
		dispatch_cv.wait(lock);
	}
	if (state == STOPPED) {
		return;
	}
	state = STOPPING;
	dispatch_cv.notify_all();
	lock.temp_unlock();
	read_service.shutdown();
	lock.temp_relock();
	while (waiters != 0 || dispatch_pumping) {
		dispatch_cv.wait(lock);
	}
	_drain_locked();
	for (gs_gsq::DecodeContext *decoder : idle_decoders) {
		memdelete(decoder);
		counters.decoder_destroyed++;
	}
	idle_decoders.clear();
	counters.decoders_idle = 0;
	file_path = String();
	catalog.clear();
	state = STOPPED;
	dispatch_cv.notify_all();
}

PayloadPageStore::Stats PayloadPageStore::stats() const {
	MutexLock lock(mutex);
	Stats result = counters;
	result.reads = read_service.stats();
	return result;
}

Dictionary PayloadPageStore::get_telemetry(const String &p_route) const {
	MutexLock lock(mutex); // Established store -> read-service lock order.
	const PayloadReadService::Stats reads = read_service.stats();
	Dictionary out;
	out["route"] = p_route;
	out["source_generation"] = counters.collection_epoch ? Variant(int64_t(counters.collection_epoch)) : Variant();
	out["collection_epoch"] = counters.collection_epoch ? Variant(int64_t(counters.collection_epoch)) : Variant();
	out["transport_generation"] = int64_t(counters.source_generation);
	out["stage_time_scope"] = "worker_cpu_sum";
	out["read_time_scope"] = "reader_wall_sum";
	out["verify_scope"] = p_route == "hlod" ? "gsq_header_and_stored_digest_inside_v4_record" : "gsq_header_and_stored_digest";
	struct Counter {
		const char *name;
		uint64_t Stats::*member;
	};
	const Counter fields[] = {
		{ "captures", &Stats::captures }, { "captures_failed", &Stats::captures_failed },
		{ "captures_on_worker_threads", &Stats::captures_on_worker_threads }, { "captures_on_main_thread", &Stats::captures_on_main_thread },
		{ "pages_decoded", &Stats::pages_decoded }, { "splats_decoded", &Stats::splats_decoded },
		{ "pages_decoded_on_worker_threads", &Stats::pages_decoded_on_worker_threads },
		{ "pages_decoded_on_main_thread", &Stats::pages_decoded_on_main_thread },
		{ "page_failures", &Stats::page_failures }, { "stored_bytes_read", &Stats::stored_bytes_read },
		{ "digest_verification_failures", &Stats::digest_verification_failures },
		{ "verify_nsec", &Stats::verify_nsec }, { "decode_nsec", &Stats::decode_nsec },
		{ "pages_decoded_digest_checked", &Stats::pages_decoded_digest_checked },
		{ "pages_decoded_digest_skipped", &Stats::pages_decoded_digest_skipped },
#ifdef TESTS_ENABLED
		{ "test_decode_delay_calls", &Stats::test_decode_delay_calls }, { "test_decode_delay_nsec", &Stats::test_decode_delay_nsec },
#endif
	};
	for (const Counter &field : fields) { out[field.name] = int64_t(counters.*field.member - collection_base.*field.member); }
	const uint64_t checked = counters.pages_decoded_digest_checked - collection_base.pages_decoded_digest_checked;
	const uint64_t skipped = counters.pages_decoded_digest_skipped - collection_base.pages_decoded_digest_skipped;
	const uint64_t digest = counters.digest_nsec - collection_base.digest_nsec;
	out["digest_status"] = checked ? (skipped ? "mixed" : "checked") : (skipped ? "skipped" : "not_performed");
	out["digest_nsec"] = checked ? Variant(int64_t(digest)) : Variant();
	out["decode_remainder_nsec"] = int64_t(counters.decode_nsec - collection_base.decode_nsec -
			(counters.verify_nsec - collection_base.verify_nsec) - digest);
	out["last_failure_reason"] = (counters.captures_failed != collection_base.captures_failed ||
			counters.page_failures != collection_base.page_failures) ? counters.last_failure_reason : String();
	out["read_requests"] = int64_t(reads.submitted - collection_base.reads.submitted);
	out["read_latency_nsec"] = int64_t(reads.read_nsec - collection_base.reads.read_nsec);
	out["read_queue_nsec"] = int64_t(reads.queue_nsec - collection_base.reads.queue_nsec);
	out["read_failures"] = int64_t(reads.completed_failed - collection_base.reads.completed_failed);
	// Legacy chunk K9 aliases, taken from this SAME locked snapshot.
	out["reads_submitted"] = int64_t(reads.submitted - collection_base.reads.submitted);
	out["reads_completed_ok"] = int64_t(reads.completed_ok - collection_base.reads.completed_ok);
	out["reads_completed_failed"] = int64_t(reads.completed_failed - collection_base.reads.completed_failed);
	out["reads_refused_busy"] = int64_t(reads.refused_busy - collection_base.reads.refused_busy);
	out["read_bytes"] = int64_t(reads.bytes_read - collection_base.reads.bytes_read);
	out["read_handle_opens"] = int64_t(reads.handle_opens - collection_base.reads.handle_opens);
	out["cancellations"] = int64_t(reads.cancelled_queued - collection_base.reads.cancelled_queued +
			reads.cancelled_reading - collection_base.reads.cancelled_reading + reads.cancelled_ready - collection_base.reads.cancelled_ready);
	out["retained_completion_peak_bytes"] = int64_t(reads.collection_peak_ready_bytes);
	out["retained_completion_peak_scope"] = "read_service_ready_only";
	out["stored_stash_bytes_peak"] = int64_t(counters.peak_stash_bytes);
	out["stored_stash_bytes_peak_scope"] = "page_store_lifetime";
	Dictionary queues, retained;
	retained["owner"] = "payload_read_service";
	retained["bytes"] = int64_t(reads.bytes_in_flight);
	retained["peak_bytes"] = int64_t(reads.collection_peak_bytes);
	retained["byte_bound"] = int64_t(config.max_bytes_in_flight);
	retained["jobs"] = int64_t(reads.queued + reads.reading + reads.ready);
	retained["peak_jobs"] = int64_t(reads.collection_peak_jobs);
	retained["job_bound"] = Variant();
	retained["job_bound_reason"] = "read service uses byte admission; no independent request-count limit";
	queues["retained"] = retained;
	out["queue_observations"] = queues;
#ifdef TESTS_ENABLED
	out["test_read_delay_calls"] = int64_t(reads.test_read_delay_calls - collection_base.reads.test_read_delay_calls);
	out["test_read_delay_nsec"] = int64_t(reads.test_read_delay_nsec - collection_base.reads.test_read_delay_nsec);
#endif
	return out;
}

bool PayloadPageStore::uses_retained_admission() const {
	MutexLock lock(mutex);
	return state == RUNNING && config.admission_policy == ADMIT_RETAINED_BYTES;
}

Error PayloadPageStore::lease_decoder(DecoderLease &r_lease, bool p_retain_on_return) {
	r_lease.reset();
	MutexLock lock(mutex);
	if (state != RUNNING) {
		return ERR_UNAVAILABLE;
	}
	if (idle_decoders.is_empty()) {
		r_lease.decoder = memnew(gs_gsq::DecodeContext);
		if (r_lease.decoder == nullptr) {
			return ERR_OUT_OF_MEMORY;
		}
		counters.decoder_created++;
	} else {
		r_lease.decoder = idle_decoders[idle_decoders.size() - 1u];
		idle_decoders.resize(idle_decoders.size() - 1u);
		counters.decoder_reuses++;
	}
	r_lease.owner = this;
	r_lease.retain_on_return = p_retain_on_return;
	counters.decoder_leases++;
	counters.decoders_leased++;
	counters.decoders_idle = idle_decoders.size();
	counters.peak_decoders_leased = MAX(counters.peak_decoders_leased, counters.decoders_leased);
	return OK;
}

void PayloadPageStore::_return_decoder(gs_gsq::DecodeContext *p_decoder, bool p_retain) {
	MutexLock lock(mutex);
	counters.decoder_returns++;
	counters.decoders_leased--;
	if (p_retain && state == RUNNING && idle_decoders.size() < config.decoder_retention_cap) {
		idle_decoders.push_back(p_decoder);
	} else {
		memdelete(p_decoder);
		counters.decoder_destroyed++;
	}
	counters.decoders_idle = idle_decoders.size();
}

Error PayloadPageStore::decode_gsq_page(const StoredPage &p_page, DecoderLease &p_decoder, bool p_verify_decoded_digest,
		DecodedPage &r_page, String &r_reason) {
	r_page.gaussians.clear();
	r_page.sh_high_order.clear();
	r_page.digest_status = DIGEST_NOT_PERFORMED;
	r_reason = String();
	const PageDescriptor &page = p_page.descriptor;
	bool valid_owner;
	{
		MutexLock lock(mutex);
		valid_owner = state == RUNNING && p_page.owner == this && p_decoder.owner == this &&
				p_page.identity.source_generation == source_generation && p_decoder.decoder != nullptr;
		counters.stored_bytes_read += uint64_t(p_page.bytes.size());
	}
	const auto decode_begin = std::chrono::steady_clock::now();
	Error error = OK;
	if (!valid_owner || page.encoding != GSQ_V1) {
		r_reason = "gsq_page_store_invalid_owner";
		error = ERR_INVALID_PARAMETER;
	} else if (p_page.error != OK || uint64_t(p_page.bytes.size()) != page.stored_bytes) {
		r_reason = vformat("gsq_read_failed: page %d (error %d, %d of %d bytes)", page.id, int(p_page.error),
				int64_t(p_page.bytes.size()), page.stored_bytes);
		error = p_page.error != OK ? p_page.error : ERR_FILE_CORRUPT;
	}
	const uint8_t *bytes = p_page.bytes.ptr();
	String page_reason;
	gs_gsq::PageInfo info;
	if (error == OK) {
		error = gs_gsq::verify_page(bytes, page.stored_bytes, info, page_reason);
		if (error != OK) {
			r_reason = vformat("page %d: %s", page.id, page_reason);
		}
	}
	const uint64_t verify_nsec = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - decode_begin)
					.count());
	if (error == OK) {
		bool matches = info.splat_count == page.splat_count && info.stored_bytes == page.stored_bytes &&
				info.sh_degree == page.sh_degree && info.opacity_bits == page.codec_params.opacity_bits &&
				memcmp(info.stored_digest, page.stored_digest, gs_gsq::kDigestBytes) == 0 &&
				memcmp(info.position_min, page.extent, sizeof(float) * 3u) == 0 &&
				memcmp(info.position_max, page.extent + 3, sizeof(float) * 3u) == 0;
		for (int i = 0; i < 3; i++) {
			matches = matches && info.sh_bits[i] == page.codec_params.sh_bits[i];
		}
		if (!matches) {
			r_reason = vformat("gsq_page_mismatch: page %d differs from its page-table entry or the codec block", page.id);
			error = ERR_FILE_CORRUPT;
		}
	}
	const uint32_t high_order = gs_gsq::sh_high_order_for_degree(page.sh_degree);
	if (error == OK) {
		r_page.gaussians.resize(page.splat_count);
		r_page.sh_high_order.resize(page.splat_count * high_order);
#ifdef TESTS_ENABLED
		_test_delay_decode();
#endif
		error = p_decoder.decoder->decode_range(bytes, page.stored_bytes, 0, page.splat_count, r_page.gaussians.ptr(),
				high_order > 0u ? r_page.sh_high_order.ptr() : nullptr, page_reason, false);
		if (error != OK) {
			r_reason = vformat("page %d: %s", page.id, page_reason);
		}
	}
	uint64_t digest_nsec = 0;
	if (error == OK && p_verify_decoded_digest) {
		uint8_t digest[gs_gsq::kDigestBytes];
		const auto digest_begin = std::chrono::steady_clock::now();
		gs_gsq::decoded_digest(r_page.gaussians.ptr(), high_order > 0u ? r_page.sh_high_order.ptr() : nullptr,
				page.splat_count, page.sh_degree, digest);
		digest_nsec = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - digest_begin)
						.count());
		if (memcmp(digest, page.decoded_digest, gs_gsq::kDigestBytes) != 0) {
			r_reason = vformat("gsq_decoded_digest_mismatch: page %d", page.id);
			error = ERR_FILE_CORRUPT;
		}
	}
	const uint64_t decode_nsec = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - decode_begin)
					.count());
	if (error != OK) {
		r_page.gaussians.clear();
		r_page.sh_high_order.clear();
		MutexLock lock(mutex);
		counters.page_failures++;
		if (error == ERR_FILE_CORRUPT) { counters.digest_verification_failures++; }
		counters.last_failure_reason = r_reason;
		return error;
	}
	r_page.digest_status = p_verify_decoded_digest ? DIGEST_CHECKED : DIGEST_SKIPPED;
	last_capture_decode_nsec += decode_nsec;
	{
		MutexLock lock(mutex);
		counters.pages_verified++;
		counters.pages_decoded++;
		counters.splats_decoded += page.splat_count;
		counters.decode_nsec += decode_nsec;
		counters.verify_nsec += verify_nsec;
		counters.digest_nsec += digest_nsec;
		if (p_verify_decoded_digest) {
			counters.pages_decoded_digest_checked++;
		} else {
			counters.pages_decoded_digest_skipped++;
		}
		if (Thread::is_main_thread()) {
			counters.pages_decoded_on_main_thread++;
		} else {
			counters.pages_decoded_on_worker_threads++;
		}
	}
	return OK;
}

Error PayloadPageStore::decode_gsq_subrange(const StoredPage &p_page, DecoderLease &p_decoder, uint32_t p_offset,
		uint32_t p_count, uint32_t p_sh_degree, Gaussian *r_gaussians, Vector3 *r_sh, String &r_reason) {
	r_reason = String();
	bool valid_owner;
	{
		MutexLock lock(mutex);
		valid_owner = state == RUNNING && p_page.owner == this && p_decoder.owner == this &&
				p_page.identity.source_generation == source_generation && p_decoder.decoder != nullptr;
		counters.stored_bytes_read += uint64_t(p_page.bytes.size());
	}
	const auto decode_begin = std::chrono::steady_clock::now();
	const uint64_t stored = uint64_t(p_page.bytes.size());
	const uint32_t high_order = gs_gsq::sh_high_order_for_degree(p_sh_degree);
	Error error = OK;
	if (!valid_owner || p_page.descriptor.encoding != OPAQUE_BYTES || p_count == 0 ||
			p_count > gs_gsq::kMaxPageSplats || p_sh_degree > gs_gsq::kMaxShDegree ||
			p_count != p_page.descriptor.splat_count || p_sh_degree != p_page.descriptor.sh_degree ||
			r_gaussians == nullptr || (high_order != 0 && r_sh == nullptr) ||
			p_offset > stored || stored - p_offset < gs_gsq::kPageHeaderBytes) {
		r_reason = "gsq_page_store_invalid_owner_or_subrange";
		error = ERR_INVALID_PARAMETER;
	} else if (p_page.error != OK || stored != p_page.descriptor.stored_bytes) {
		r_reason = "gsq_page_store_incomplete_record";
		error = p_page.error != OK ? p_page.error : ERR_FILE_CORRUPT;
	}
	const uint8_t *bytes = error == OK ? p_page.bytes.ptr() + p_offset : nullptr;
	const uint64_t page_bytes = error == OK ? stored - p_offset : 0;
	gs_gsq::PageInfo info;
	if (error == OK) {
		error = gs_gsq::verify_page(bytes, page_bytes, info, r_reason);
	}
	const uint64_t verify_nsec = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - decode_begin).count());
	if (error == OK && (info.splat_count != p_count || info.sh_degree != p_sh_degree ||
			info.sh_high_order != high_order || uint64_t(info.stored_bytes) != page_bytes)) {
		r_reason = "gsq_page_layout_mismatch";
		error = ERR_FILE_CORRUPT;
	}
	if (error == OK) {
		// Verification is already measured; this delay belongs only to decode.
#ifdef TESTS_ENABLED
		_test_delay_decode();
#endif
		error = p_decoder.decoder->decode_range(bytes, page_bytes, 0, p_count, r_gaussians,
				high_order != 0 ? r_sh : nullptr, r_reason, false);
	}
	const uint64_t decode_nsec = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - decode_begin).count());
	if (error != OK) {
		MutexLock lock(mutex);
		counters.page_failures++;
		if (error == ERR_FILE_CORRUPT) { counters.digest_verification_failures++; }
		counters.last_failure_reason = r_reason;
		return error;
	}
	last_capture_decode_nsec += decode_nsec;
	{
		MutexLock lock(mutex);
		counters.pages_verified++;
		counters.pages_decoded++;
		counters.splats_decoded += p_count;
		counters.decode_nsec += decode_nsec;
		counters.verify_nsec += verify_nsec;
		// This is V4's existing policy: outer and GSQ stored digests, decoded
		// semantics/bounds/metrics in its adapter, no canonical decoded digest.
		counters.pages_decoded_digest_skipped++;
		if (Thread::is_main_thread()) {
			counters.pages_decoded_on_main_thread++;
		} else {
			counters.pages_decoded_on_worker_threads++;
		}
	}
	return OK;
}

void PayloadPageStore::begin_capture(bool p_reset_decode_span) {
	if (p_reset_decode_span) {
		last_capture_decode_nsec = 0;
	}
	MutexLock lock(mutex);
	counters.captures++;
	if (Thread::is_main_thread()) { ++counters.captures_on_main_thread; }
	else { ++counters.captures_on_worker_threads; }
}

void PayloadPageStore::end_capture(bool p_success, const String &p_reason) {
	if (!p_success) {
		MutexLock lock(mutex);
		counters.captures_failed++;
		// The V4 adapter verifies its outer stored digest before GSQ decode.
		// Inner GSQ failures are already counted by decode_gsq_* above.
		if (p_reason == "v4_payload_digest_mismatch") { counters.digest_verification_failures++; }
		counters.last_failure_reason = p_reason;
	}
}

uint64_t PayloadPageStore::take_thread_last_capture_decode_nsec() {
	const uint64_t nsec = last_capture_decode_nsec;
	last_capture_decode_nsec = 0;
	return nsec;
}

#ifdef TESTS_ENABLED
Error PayloadPageStore::_test_set_telemetry_delays(uint32_t p_read_usec, uint32_t p_decode_usec) {
	MutexLock lock(mutex);
	if (state != RUNNING) { return ERR_UNAVAILABLE; }
	if (!requests.is_empty() || counters.decoders_leased != 0) { return ERR_BUSY; }
	const Error error = read_service._test_set_read_delay(p_read_usec);
	if (error != OK) { return error; }
	test_decode_delay_usec = p_decode_usec;
	return OK;
}

void PayloadPageStore::_test_delay_decode() {
	uint32_t usec;
	{ MutexLock lock(mutex); usec = test_decode_delay_usec; }
	if (usec == 0) { return; }
	const auto started = std::chrono::steady_clock::now();
	OS::get_singleton()->delay_usec(usec);
	const uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
	MutexLock lock(mutex);
	++counters.test_decode_delay_calls;
	counters.test_decode_delay_nsec += elapsed;
}
#endif

#ifdef TESTS_ENABLED
Error PayloadPageStore::_test_set_fault(TestFault p_fault) {
	MutexLock lock(mutex);
	if (state != RUNNING || counters.live_reservations != 0 || dispatch_pumping || waiters != 0) {
		return ERR_BUSY;
	}
	if (p_fault < TEST_FAULT_NONE || p_fault > TEST_FAULT_SWAP_REQUESTER_TICKETS) {
		return ERR_INVALID_PARAMETER;
	}
	test_fault = p_fault;
	test_fault_count = 0;
	test_first_ticket = 0;
	test_second_ticket = 0;
	return OK;
}

bool PayloadPageStore::_test_dispatch_one_completion() {
	MutexLock lock(mutex);
	if (state != RUNNING || dispatch_pumping || counters.service_bytes == 0) {
		return false;
	}
	waiters++;
	const bool received = _pump_locked(lock, 0);
	waiters--;
	dispatch_cv.notify_all();
	return received;
}

uint64_t PayloadPageStore::_test_faults_triggered() const {
	MutexLock lock(mutex);
	return test_fault_count;
}
#endif
