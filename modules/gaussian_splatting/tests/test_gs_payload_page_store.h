#pragma once

#include "test_macros.h"

#include "../io/gs_payload_page_store.h"
#include "gs_payload_page_store_test_fixture.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/thread.h"

#include <cstring>
#include <type_traits>
#include <utility>

namespace TestPayloadPageStoreContracts {

using Store = PayloadPageStore;
using Read = PayloadReadService;

static_assert(!std::is_copy_constructible<Store::StoredPage>::value, "stored reservations must have one owner");
static_assert(!std::is_copy_assignable<Store::StoredPage>::value, "stored reservations must not be copied");
static_assert(!std::is_copy_constructible<Store::DecoderLease>::value, "decoder leases must be exclusive");

struct RawFile {
	String path;
	Vector<uint8_t> bytes;
	bool valid = false;

	explicit RawFile(const String &p_suffix, uint8_t p_salt = 0) {
		path = OS::get_singleton()->get_temp_path().path_join("gs_page_store_" + p_suffix + "_" + itos(OS::get_singleton()->get_ticks_usec()) + ".bin");
		bytes.resize(32768);
		for (int64_t i = 0; i < bytes.size(); i++) {
			bytes.ptrw()[i] = uint8_t(i * 31u + (i >> 7) * 13u + (i >> 13) * 7u + p_salt);
		}
		valid = write();
	}

	bool write() const {
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		if (file.is_null()) {
			return false;
		}
		file->store_buffer(bytes.ptr(), bytes.size());
		file->flush();
		return file->get_error() == OK;
	}

	~RawFile() {
		DirAccess::remove_absolute(path);
	}
};

inline LocalVector<Store::PageDescriptor> raw_descriptors(uint32_t p_count = 3, uint32_t p_size = 64) {
	LocalVector<Store::PageDescriptor> descriptors;
	for (uint32_t i = 0; i < p_count; i++) {
		Store::PageDescriptor page;
		page.id = 100u + i;
		page.offset = 41u + i * 701u;
		page.stored_bytes = p_size;
		descriptors.push_back(page);
	}
	return descriptors;
}

inline bool install(Store &p_store, const String &p_path, const LocalVector<Store::PageDescriptor> &p_pages,
		const Store::Config &p_config = Store::Config(), uint64_t p_generation = 7) {
	if (p_store.start(p_config) != OK) {
		FAIL("PayloadPageStore must start; phase-1 stub intentionally refuses this precondition");
		return false;
	}
	if (p_store.replace_source(p_path, p_generation, p_pages) != OK) {
		FAIL("PayloadPageStore must install an immutable descriptor catalog");
		return false;
	}
	return true;
}

inline bool request(Store &p_store, uint64_t p_requester, uint64_t p_ticket, const Store::PageDescriptor &p_page) {
	if (p_store.request(p_requester, p_ticket, p_page) != Read::SUBMIT_OK) {
		FAIL("valid positional request was refused");
		return false;
	}
	return true;
}

inline bool take(Store &p_store, uint64_t p_requester, uint64_t p_ticket, Store::StoredPage &r_page) {
	if (!p_store.wait(p_requester, p_ticket, r_page) || !r_page.is_valid()) {
		FAIL("accepted live request must deliver a completion");
		return false;
	}
	return true;
}

// This checker is also the mutation oracle. It never asks the implementation
// which bytes/identity it should expect, and never writes doctest assertions.
inline bool bytes_match(const Store::StoredPage &p_page, const RawFile &p_file, const Store::PageDescriptor &p_expected,
		uint64_t p_requester, uint64_t p_ticket, uint64_t p_generation = 7) {
	const Store::Identity identity = p_page.get_identity();
	if (!p_page.is_valid() || p_page.get_error() != OK || identity.source_generation != p_generation ||
			identity.requester != p_requester || identity.ticket != p_ticket || identity.page_id != p_expected.id ||
			uint64_t(p_page.get_bytes().size()) != p_expected.stored_bytes ||
			p_expected.offset + p_expected.stored_bytes > uint64_t(p_file.bytes.size())) {
		return false;
	}
	return memcmp(p_page.get_bytes().ptr(), p_file.bytes.ptr() + p_expected.offset, p_expected.stored_bytes) == 0;
}

inline void check_drained(const Store::Stats &p_stats, uint64_t p_reservations) {
	CHECK(p_stats.service_bytes == 0);
	CHECK(p_stats.reads.bytes_in_flight == 0);
	CHECK(p_stats.stash_bytes == 0);
	CHECK(p_stats.caller_bytes == 0);
	CHECK(p_stats.retained_bytes == 0);
	CHECK(p_stats.live_reservations == 0);
	CHECK(p_stats.reservations_acquired == p_reservations);
	CHECK(p_stats.reservations_released == p_reservations);
}

inline LocalVector<Store::PageDescriptor> gsq_descriptors(const TestPayloadPageStore::Fixture &p_fixture) {
	LocalVector<Store::PageDescriptor> descriptors;
	for (uint32_t i = 0; i < p_fixture.pages.size(); i++) {
		const GsqPagedChunkPayloadSource::Page &page = p_fixture.pages[i];
		Store::PageDescriptor descriptor;
		descriptor.id = 1000u + i;
		descriptor.offset = page.offset;
		descriptor.stored_bytes = page.stored_bytes;
		descriptor.encoding = Store::GSQ_V1;
		descriptor.splat_count = page.splat_count;
		descriptor.sh_degree = p_fixture.sh_degree;
		descriptor.codec_params = p_fixture.codec_params;
		memcpy(descriptor.stored_digest, page.stored_digest, sizeof(descriptor.stored_digest));
		memcpy(descriptor.decoded_digest, page.decoded_digest, sizeof(descriptor.decoded_digest));
		memcpy(descriptor.extent, page.extent, sizeof(descriptor.extent));
		descriptors.push_back(descriptor);
	}
	return descriptors;
}

inline bool capture_page(Store &p_store, const Store::PageDescriptor &p_descriptor, uint64_t p_ticket,
		bool p_verify_digest, Store::DecodedPage &r_decoded, Error &r_decode_error, String &r_reason) {
	p_store.begin_capture();
	if (!request(p_store, 42, p_ticket, p_descriptor)) {
		return false;
	}
	Store::StoredPage stored;
	if (!take(p_store, 42, p_ticket, stored)) {
		return false;
	}
	Store::DecoderLease decoder;
	if (p_store.lease_decoder(decoder) != OK || decoder.get() == nullptr) {
		FAIL("a capture must lease a decoder before decoding");
		return false;
	}
	r_decode_error = p_store.decode_gsq_page(stored, decoder, p_verify_digest, r_decoded, r_reason);
	p_store.end_capture(r_decode_error == OK, r_reason);
	return true;
}

struct DispatchReport {
	bool accounting_ok = true;
	bool routing_ok = true;
	uint64_t faults_triggered = 0;
};

// One reader gives a known order without sleeps or polling: pump one actual
// completion at a time through the same stash handoff used by wait(). The two
// requesters use distinct local tickets here so swapping either the ticket or
// the whole routing key is observable. The healthy interleaving case also
// exercises identical local tickets belonging to different requesters.
inline bool dispatch_contract(const RawFile &p_file, Store::TestFault p_fault, DispatchReport &r_report) {
	Store store;
	Store::Config config;
	config.reader_count = 1;
	const LocalVector<Store::PageDescriptor> pages = raw_descriptors(2);
	if (!install(store, p_file.path, pages, config)) {
		return false;
	}
	if (store._test_set_fault(p_fault) != OK) {
		FAIL("test fault configuration was unavailable");
		return false;
	}
	if (!request(store, 10, 9, pages[0]) || !request(store, 20, 19, pages[1])) {
		return false;
	}
	for (uint64_t pumped = 1; pumped <= 2; pumped++) {
		if (!store._test_dispatch_one_completion()) {
			FAIL("accepted completion never reached dispatch");
			return false;
		}
		const Store::Stats stats = store.stats();
		r_report.accounting_ok = r_report.accounting_ok && stats.service_bytes == (2 - pumped) * 64 &&
				stats.reads.bytes_in_flight == stats.service_bytes && stats.stash_bytes == pumped * 64 &&
				stats.retained_bytes == 128 && stats.caller_bytes == 0 &&
				stats.reservations_acquired == 2 && stats.reservations_released == 0 && stats.live_reservations == 2;
	}
	Store::StoredPage a;
	Store::StoredPage b;
	// A fault may cause a safe rejection. That is a routing-contract violation,
	// too: both known, accepted requests must succeed on the healthy path.
	const bool a_received = store.wait(10, 9, a);
	const bool b_received = store.wait(20, 19, b);
	r_report.routing_ok = a_received && b_received && bytes_match(a, p_file, pages[0], 10, 9) && bytes_match(b, p_file, pages[1], 20, 19);
	a.reset();
	b.reset();
	r_report.faults_triggered = store._test_faults_triggered();
	return true;
}

struct WorkerDecode {
	Store *store = nullptr;
	Store::PageDescriptor descriptor;
	Store::DecodedPage output;
	String reason;
	Error error = ERR_BUG;
	bool transport_ok = false;
	bool on_main_thread = true;
	uint64_t decode_nsec = 0;
	uint64_t second_take_nsec = 1;

	static void run(void *p_user) {
		WorkerDecode *self = static_cast<WorkerDecode *>(p_user);
		self->on_main_thread = Thread::is_main_thread();
		self->store->begin_capture();
		Store::StoredPage stored;
		Store::DecoderLease decoder;
		self->transport_ok = self->store->request(42, 3, self->descriptor) == Read::SUBMIT_OK &&
				self->store->wait(42, 3, stored) && stored.is_valid() &&
				self->store->lease_decoder(decoder) == OK && decoder.get() != nullptr;
		if (!self->transport_ok) {
			self->store->end_capture(false, "worker transport unavailable");
			return;
		}
		self->error = self->store->decode_gsq_page(stored, decoder, true, self->output, self->reason);
		self->store->end_capture(self->error == OK, self->reason);
		self->decode_nsec = self->store->take_thread_last_capture_decode_nsec();
		self->second_take_nsec = self->store->take_thread_last_capture_decode_nsec();
	}
};

} // namespace TestPayloadPageStoreContracts

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] concurrent positional reads return exact bytes") {
	using namespace TestPayloadPageStoreContracts;
	RawFile file("offsets");
	if (!file.valid) {
		FAIL("could not write deterministic positional-read fixture");
		return;
	}
	Store store;
	LocalVector<Store::PageDescriptor> pages = raw_descriptors(8);
	for (uint32_t i = 0; i < pages.size(); i++) {
		pages[i].stored_bytes += i * 13;
	}
	if (!install(store, file.path, pages)) {
		return;
	}
	uint64_t total_bytes = 0;
	for (uint32_t i = 0; i < pages.size(); i++) {
		if (!request(store, 1, i + 1, pages[i])) {
			return;
		}
		total_bytes += pages[i].stored_bytes;
	}
	CHECK(store.stats().retained_bytes == total_bytes);
	// Reverse consumption forces foreign completions through dispatch/stash.
	for (uint32_t i = pages.size(); i > 0; i--) {
		Store::StoredPage stored;
		if (!take(store, 1, i, stored)) {
			return;
		}
		CHECK(bytes_match(stored, file, pages[i - 1], 1, i));
	}
	const Store::Stats stats = store.stats();
	check_drained(stats, 8);
	CHECK(stats.reads.bytes_read == total_bytes);
	CHECK(stats.reads.completed_ok == 8);
	CHECK(stats.reads.completed_failed == 0);
	CHECK(stats.reads.open_handles == 0);
	CHECK(stats.peak_retained_bytes == total_bytes);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] interleaved requesters retain their own tickets") {
	using namespace TestPayloadPageStoreContracts;
	RawFile file("routing");
	if (!file.valid) {
		FAIL("could not write routing fixture");
		return;
	}
	// Healthy run exercises the exact checker used by both failure controls.
	DispatchReport report;
	if (!dispatch_contract(file, Store::TEST_FAULT_NONE, report)) {
		return;
	}
	CHECK(report.accounting_ok);
	CHECK(report.routing_ok);
	CHECK(report.faults_triggered == 0);

	Store store;
	Store::Config config;
	config.reader_count = 1;
	const LocalVector<Store::PageDescriptor> pages = raw_descriptors(6);
	if (!install(store, file.path, pages, config)) {
		return;
	}
	const uint64_t requesters[] = { 11, 22, 33, 11, 22, 33 };
	const uint64_t tickets[] = { 1, 1, 1, 2, 2, 2 };
	for (uint32_t i = 0; i < 6; i++) {
		if (!request(store, requesters[i], tickets[i], pages[i])) {
			return;
		}
	}
	const uint32_t order[] = { 5, 0, 4, 1, 3, 2 };
	for (uint32_t index : order) {
		Store::StoredPage stored;
		if (!take(store, requesters[index], tickets[index], stored)) {
			return;
		}
		CHECK(bytes_match(stored, file, pages[index], requesters[index], tickets[index]));
	}
	check_drained(store.stats(), 6);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] service stash and caller bytes share one reservation") {
	using namespace TestPayloadPageStoreContracts;
	RawFile file("accounting");
	if (!file.valid) {
		FAIL("could not write accounting fixture");
		return;
	}
	Store store;
	Store::Config config;
	config.reader_count = 1;
	config.max_bytes_in_flight = 160; // Fixture capacity, not a runtime default.
	LocalVector<Store::PageDescriptor> pages = raw_descriptors();
	pages[1].stored_bytes = 96;
	pages[2].stored_bytes = 1;
	if (!install(store, file.path, pages, config) || !request(store, 10, 1, pages[0]) || !request(store, 20, 1, pages[1])) {
		return;
	}
	Store::Stats stats = store.stats();
	CHECK(stats.service_bytes == 160);
	CHECK(stats.retained_bytes == 160);
	if (!store._test_dispatch_one_completion()) {
		FAIL("the first read must complete before the accounting snapshot");
		return;
	}
	stats = store.stats();
	CHECK(stats.service_bytes == 96);
	CHECK(stats.reads.bytes_in_flight == 96);
	CHECK(stats.stash_bytes == 64);
	CHECK(stats.retained_bytes == 160);
	CHECK(stats.peak_service_bytes == 160);
	CHECK(stats.peak_stash_bytes == 64);
	CHECK(stats.peak_retained_bytes == 160);
	CHECK(store.request(30, 1, pages[2]) == Read::SUBMIT_BUSY);
	Store::StoredPage stored;
	if (!take(store, 20, 1, stored)) {
		return;
	}
	stats = store.stats();
	CHECK(stats.service_bytes == 0);
	CHECK(stats.stash_bytes == 64);
	CHECK(stats.caller_bytes == 96);
	CHECK(stats.retained_bytes == 160);
	CHECK(stats.reservations_released == 0);
	CHECK(store.request(30, 1, pages[2]) == Read::SUBMIT_BUSY);
	Store::StoredPage moved(std::move(stored));
	CHECK_FALSE(stored.is_valid());
	CHECK(bytes_match(moved, file, pages[1], 20, 1));
	stored.reset(); // Moved-from reset cannot release the transferred reservation.
	CHECK(store.stats().reservations_released == 0);
	moved.reset();
	moved.reset();
	CHECK(store.stats().reservations_released == 1);
	if (!request(store, 30, 1, pages[2])) {
		return;
	}
	Store::StoredPage first_page;
	Store::StoredPage tiny_page;
	if (!take(store, 10, 1, first_page) || !take(store, 30, 1, tiny_page)) {
		return;
	}
	CHECK(bytes_match(first_page, file, pages[0], 10, 1));
	CHECK(bytes_match(tiny_page, file, pages[2], 30, 1));
	CHECK(store.stats().retained_bytes == 65);
	first_page = std::move(tiny_page); // Replacing an owner releases only its old reservation.
	CHECK_FALSE(tiny_page.is_valid());
	CHECK(store.stats().reservations_released == 2);
	CHECK(store.stats().retained_bytes == 1);
	CHECK(bytes_match(first_page, file, pages[2], 30, 1));
	tiny_page.reset();
	CHECK(store.stats().reservations_released == 2);
	first_page.reset();
	check_drained(store.stats(), 3);
	CHECK(store.stats().peak_retained_bytes == 160);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] cancellation replacement and shutdown release each owner once") {
	using namespace TestPayloadPageStoreContracts;
	RawFile file("drain_old");
	RawFile replacement("drain_new", 73);
	if (!file.valid || !replacement.valid) {
		FAIL("could not write source replacement fixtures");
		return;
	}
	Store store;
	Store::Config config;
	config.reader_count = 1;
	const LocalVector<Store::PageDescriptor> pages = raw_descriptors();
	if (!install(store, file.path, pages, config) || !request(store, 10, 1, pages[0]) ||
			!request(store, 10, 2, pages[1]) || !request(store, 20, 1, pages[2])) {
		return;
	}
	if (!store._test_dispatch_one_completion()) {
		FAIL("must hold a real stored completion in the stash before draining");
		return;
	}
	CHECK(store.stats().stash_bytes == 64);
	CHECK(store.stats().service_bytes == 128);
	SUBCASE("cancel one requester preserves the other requester") {
		if (store.cancel_requester(10) != OK) {
			FAIL("cancellation must synchronously drain service and stash ownership");
			return;
		}
		const Store::Stats stats = store.stats();
		CHECK(stats.stash_bytes == 0);
		CHECK(stats.service_bytes == 64);
		CHECK(stats.retained_bytes == 64);
		CHECK(stats.reservations_released == 2);
		CHECK(stats.live_reservations == 1);
		Store::StoredPage cancelled;
		CHECK_FALSE(store.wait(10, 1, cancelled));
		CHECK_FALSE(store.wait(10, 2, cancelled));
		CHECK_FALSE(cancelled.is_valid());
		CHECK(cancelled.get_bytes().is_empty());
		Store::StoredPage other;
		if (!take(store, 20, 1, other)) {
			return;
		}
		CHECK(bytes_match(other, file, pages[2], 20, 1));
		other.reset();
		CHECK(store.cancel_requester(10) == OK);
		store.shutdown();
		check_drained(store.stats(), 3);
	}
	SUBCASE("replace drains old generation and permits local ticket reuse") {
		if (store.replace_source(replacement.path, 8, pages) != OK) {
			FAIL("source replacement must drain the previous generation");
			return;
		}
		check_drained(store.stats(), 3);
		Store::StoredPage stale;
		CHECK_FALSE(store.wait(10, 1, stale));
		CHECK_FALSE(stale.is_valid());
		if (!request(store, 10, 1, pages[0])) {
			return;
		}
		Store::StoredPage fresh;
		if (!take(store, 10, 1, fresh)) {
			return;
		}
		CHECK(bytes_match(fresh, replacement, pages[0], 10, 1, 8));
		fresh.reset();
		store.shutdown();
		check_drained(store.stats(), 4);
	}
	SUBCASE("shutdown drains both owners and is idempotent") {
		store.shutdown();
		check_drained(store.stats(), 3);
		store.shutdown();
		check_drained(store.stats(), 3);
		Store::StoredPage stopped;
		CHECK_FALSE(store.wait(10, 1, stopped));
		CHECK_FALSE(stopped.is_valid());
		CHECK(store.request(20, 3, pages[0]) == Read::SUBMIT_NOT_RUNNING);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] short reads and mismatched request descriptors produce no output") {
	using namespace TestPayloadPageStoreContracts;
	RawFile file("invalid");
	if (!file.valid) {
		FAIL("could not write invalid-read fixture");
		return;
	}
	Store store;
	LocalVector<Store::PageDescriptor> pages = raw_descriptors();
	pages[2].offset = file.bytes.size() - 8;
	pages[2].stored_bytes = 16;
	if (!install(store, file.path, pages)) {
		return;
	}
	const Store::PageDescriptor installed = pages[0];
	pages[0].offset++; // Changing the caller's catalog cannot mutate the installed snapshot.
	CHECK(store.request(10, 8, pages[0]) == Read::SUBMIT_INVALID);
	Store::StoredPage mutated;
	CHECK_FALSE(store.wait(10, 8, mutated));
	CHECK_FALSE(mutated.is_valid());
	CHECK(mutated.get_bytes().is_empty());
	pages[0] = installed;
	for (int field = 0; field < 3; field++) {
		Store::PageDescriptor wrong = pages[0];
		if (field == 0) {
			wrong.id += 999;
		} else if (field == 1) {
			wrong.offset++;
		} else {
			wrong.stored_bytes++;
		}
		CHECK(store.request(10, field + 1, wrong) == Read::SUBMIT_INVALID);
		Store::StoredPage absent;
		CHECK_FALSE(store.wait(10, field + 1, absent));
		CHECK_FALSE(absent.is_valid());
		CHECK(absent.get_bytes().is_empty());
	}
	CHECK(store.stats().reservations_acquired == 0);
	if (!request(store, 10, 5, pages[0])) {
		return;
	}
	Store::StoredPage previous;
	if (!take(store, 10, 5, previous)) {
		return;
	}
	CHECK(bytes_match(previous, file, pages[0], 10, 5));
	// A failed wait must not expose bytes left by an earlier successful wait.
	CHECK_FALSE(store.wait(10, 999, previous));
	CHECK_FALSE(previous.is_valid());
	CHECK(previous.get_bytes().is_empty());
	check_drained(store.stats(), 1);
	if (!request(store, 10, 4, pages[2])) {
		return;
	}
	Store::StoredPage short_read;
	if (!take(store, 10, 4, short_read)) {
		return;
	}
	CHECK(short_read.get_error() == ERR_FILE_EOF);
	CHECK(short_read.get_bytes().is_empty());
	CHECK(short_read.get_identity().page_id == pages[2].id);
	short_read.reset();
	CHECK(store.stats().reads.completed_failed == 1);
	check_drained(store.stats(), 2);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] decoder leases are exclusive returned capped and counted") {
	using namespace TestPayloadPageStoreContracts;
	Store store;
	Store::Config config;
	CHECK(config.reader_count == 2);
	CHECK(config.max_bytes_in_flight == 64ull * 1024ull * 1024ull);
	CHECK(config.max_idle_handles == 0);
	CHECK(config.decoder_retention_cap == UINT32_MAX);
	config.decoder_retention_cap = 1; // Exercise an owner's cap, not a changed default.
	if (store.start(config) != OK) {
		FAIL("decoder pool requires a running store");
		return;
	}
	Store::DecoderLease first;
	Store::DecoderLease second;
	if (store.lease_decoder(first) != OK || store.lease_decoder(second) != OK || first.get() == nullptr || second.get() == nullptr) {
		FAIL("two simultaneous captures must each receive a decoder");
		return;
	}
	CHECK(first.get() != second.get());
	CHECK(store.stats().decoders_leased == 2);
	CHECK(store.stats().peak_decoders_leased == 2);
	gs_gsq::DecodeContext *returned = first.get();
	Store::DecoderLease moved(std::move(first));
	CHECK(first.get() == nullptr);
	first.reset();
	CHECK(store.stats().decoder_returns == 0);
	moved.reset();
	moved.reset();
	CHECK(store.stats().decoder_returns == 1);
	CHECK(store.stats().decoders_idle == 1);
	Store::DecoderLease reused;
	if (store.lease_decoder(reused) != OK || reused.get() == nullptr) {
		FAIL("a returned decoder must be leasable again");
		return;
	}
	CHECK(reused.get() == returned);
	reused.reset();
	second.reset(); // Idle cap already full: this context must be freed.
	const Store::Stats stats = store.stats();
	CHECK(stats.decoder_leases == 3);
	CHECK(stats.decoder_returns == 3);
	CHECK(stats.decoder_created == 2);
	CHECK(stats.decoder_reuses == 1);
	CHECK(stats.decoder_destroyed == 1);
	CHECK(stats.decoders_leased == 0);
	CHECK(stats.decoders_idle == 1);
	store.shutdown();
	CHECK(store.stats().decoders_idle == 0);
	CHECK(store.stats().decoder_destroyed == 2);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] GSQ stage counters preserve checked and skipped digest meanings") {
	using namespace TestPayloadPageStoreContracts;
	TestPayloadPageStore::Fixture fixture("store_stages");
	if (!fixture.valid || fixture.pages.is_empty() || fixture.reference_digests.is_empty()) {
		FAIL("deterministic GSQ fixture was unavailable");
		return;
	}
	Store store;
	const LocalVector<Store::PageDescriptor> pages = gsq_descriptors(fixture);
	if (!install(store, fixture.path, pages)) {
		return;
	}
	for (uint64_t capture = 0; capture < 2; capture++) {
		const bool checked = capture == 0;
		Store::DecodedPage decoded;
		Error error = ERR_BUG;
		String reason;
		if (!capture_page(store, pages[0], capture + 1, checked, decoded, error, reason)) {
			return;
		}
		CHECK(error == OK);
		if (decoded.gaussians.size() != pages[0].splat_count || decoded.sh_high_order.size() != pages[0].splat_count * fixture.sh_high_order) {
			FAIL("successful GSQ decode must return the whole page and SH payload");
			return;
		}
		uint8_t digest[gs_gsq::kDigestBytes];
		gs_gsq::decoded_digest(decoded.gaussians.ptr(), decoded.sh_high_order.ptr(), decoded.gaussians.size(), fixture.sh_degree, digest);
		CHECK(memcmp(digest, fixture.reference_digests[0].bytes, sizeof(digest)) == 0);
		CHECK(decoded.digest_status == (checked ? Store::DIGEST_CHECKED : Store::DIGEST_SKIPPED));
		const uint64_t span = store.take_thread_last_capture_decode_nsec();
		CHECK(span > 0);
		CHECK(store.take_thread_last_capture_decode_nsec() == 0);
		CHECK(store.stats().pages_decoded_digest_checked == 1);
		CHECK(store.stats().pages_decoded_digest_skipped == capture);
	}
	WorkerDecode worker;
	worker.store = &store;
	worker.descriptor = pages[0];
	Thread thread;
	thread.start(&WorkerDecode::run, &worker);
	if (!thread.is_started()) {
		FAIL("could not start the CPU store decode worker");
		return;
	}
	thread.wait_to_finish();
	if (!worker.transport_ok || worker.output.gaussians.size() != pages[0].splat_count ||
			worker.output.sh_high_order.size() != pages[0].splat_count * fixture.sh_high_order) {
		FAIL("store worker must read and decode a whole page");
		return;
	}
	CHECK(worker.error == OK);
	CHECK_FALSE(worker.on_main_thread);
	CHECK(worker.decode_nsec > 0);
	CHECK(worker.second_take_nsec == 0);
	CHECK(store.take_thread_last_capture_decode_nsec() == 0); // Worker TLS cannot leak to main.
	CHECK(worker.output.digest_status == Store::DIGEST_CHECKED);
	uint8_t worker_digest[gs_gsq::kDigestBytes];
	gs_gsq::decoded_digest(worker.output.gaussians.ptr(), worker.output.sh_high_order.ptr(),
			worker.output.gaussians.size(), fixture.sh_degree, worker_digest);
	CHECK(memcmp(worker_digest, fixture.reference_digests[0].bytes, sizeof(worker_digest)) == 0);
	const Store::Stats stats = store.stats();
	CHECK(stats.captures == 3);
	CHECK(stats.captures_failed == 0);
	CHECK(stats.pages_verified == 3);
	CHECK(stats.pages_decoded == 3);
	CHECK(stats.splats_decoded == uint64_t(pages[0].splat_count) * 3);
	CHECK(stats.pages_decoded_digest_checked == 2);
	CHECK(stats.pages_decoded_digest_skipped == 1);
	CHECK(stats.pages_decoded_on_main_thread == 2);
	CHECK(stats.pages_decoded_on_worker_threads == 1);
	CHECK(stats.stored_bytes_read == uint64_t(pages[0].stored_bytes) * 3);
	CHECK(stats.verify_nsec > 0);
	CHECK(stats.digest_nsec > 0);
	CHECK(stats.verify_nsec + stats.digest_nsec <= stats.decode_nsec);
	CHECK(stats.page_failures == 0);
	CHECK(stats.last_failure_reason.is_empty());
	check_drained(stats, 3);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] skipped decoded digest accepts only independently verified stored bytes") {
	using namespace TestPayloadPageStoreContracts;
	TestPayloadPageStore::Fixture fixture("store_digest_policy");
	if (!fixture.valid || fixture.pages.is_empty() || fixture.reference_digests.is_empty()) {
		FAIL("deterministic digest-policy fixture was unavailable");
		return;
	}
	LocalVector<Store::PageDescriptor> pages = gsq_descriptors(fixture);
	pages[0].decoded_digest[0] ^= 1; // Stored page is valid; only expected decoded digest differs.
	Store store;
	if (!install(store, fixture.path, pages)) {
		return;
	}
	Store::DecodedPage output;
	String reason;
	Error error = ERR_BUG;
	if (!capture_page(store, pages[0], 1, false, output, error, reason)) {
		return;
	}
	CHECK(error == OK);
	if (output.gaussians.size() != pages[0].splat_count || output.sh_high_order.size() != pages[0].splat_count * fixture.sh_high_order) {
		FAIL("skipping decoded digest must still decode the verified stored page");
		return;
	}
	uint8_t digest[gs_gsq::kDigestBytes];
	gs_gsq::decoded_digest(output.gaussians.ptr(), output.sh_high_order.ptr(), output.gaussians.size(), fixture.sh_degree, digest);
	CHECK(memcmp(digest, fixture.reference_digests[0].bytes, sizeof(digest)) == 0);
	CHECK(output.digest_status == Store::DIGEST_SKIPPED);
	const Store::Stats skipped = store.stats();
	CHECK(skipped.pages_verified == 1);
	CHECK(skipped.pages_decoded_digest_checked == 0);
	CHECK(skipped.pages_decoded_digest_skipped == 1);
	CHECK(skipped.digest_nsec == 0);
	CHECK(skipped.verify_nsec > 0);
	if (!capture_page(store, pages[0], 2, true, output, error, reason)) {
		return;
	}
	CHECK(error != OK);
	CHECK(output.gaussians.is_empty());
	CHECK(output.sh_high_order.is_empty());
	CHECK(output.digest_status == Store::DIGEST_NOT_PERFORMED);
	CHECK(reason.contains("gsq_decoded_digest_mismatch"));
	const Store::Stats failed = store.stats();
	CHECK(failed.captures == 2);
	CHECK(failed.captures_failed == 1);
	CHECK(failed.page_failures == 1);
	CHECK(failed.pages_decoded == 1);
	CHECK(failed.pages_verified == 1);
	CHECK(failed.pages_decoded_digest_checked == 0);
	CHECK(failed.pages_decoded_digest_skipped == 1);
	CHECK(failed.decode_nsec == skipped.decode_nsec);
	CHECK(failed.verify_nsec == skipped.verify_nsec);
	CHECK(failed.digest_nsec == 0);
	check_drained(failed, 2);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] corrupt GSQ and descriptor mismatches fail without decoded output") {
	using namespace TestPayloadPageStoreContracts;
	TestPayloadPageStore::Fixture fixture("store_corrupt");
	if (!fixture.valid || fixture.pages.is_empty()) {
		FAIL("deterministic GSQ fixture was unavailable");
		return;
	}
	RawFile corrupt("gsq_corrupt");
	corrupt.bytes.resize(fixture.bytes.size());
	memcpy(corrupt.bytes.ptrw(), fixture.bytes.ptr(), fixture.bytes.size());
	LocalVector<Store::PageDescriptor> pages = gsq_descriptors(fixture);
	String path = fixture.path;
	String expected_reason;
	bool verify_decoded_digest = true;
	SUBCASE("stored digest mismatch") {
		const uint64_t index = pages[0].offset + gs_gsq::kPageHeaderBytes;
		if (index >= uint64_t(corrupt.bytes.size())) {
			FAIL("fixture has no stored frame byte to corrupt");
			return;
		}
		corrupt.bytes.ptrw()[index] ^= 0x40;
		if (!corrupt.write()) {
			FAIL("could not write corrupt stored-page fixture");
			return;
		}
		path = corrupt.path;
		expected_reason = "gsq_digest_mismatch";
	}
	SUBCASE("stored verification still runs when decoded digest is skipped") {
		const uint64_t index = pages[0].offset + gs_gsq::kPageHeaderBytes;
		if (index >= uint64_t(corrupt.bytes.size())) {
			FAIL("fixture has no stored frame byte to corrupt");
			return;
		}
		corrupt.bytes.ptrw()[index] ^= 0x40;
		if (!corrupt.write()) {
			FAIL("could not write corrupt stored-page fixture");
			return;
		}
		path = corrupt.path;
		expected_reason = "gsq_digest_mismatch";
		verify_decoded_digest = false;
	}
	SUBCASE("descriptor splat count mismatch") {
		pages[0].splat_count--;
		expected_reason = "gsq_page_mismatch";
	}
	SUBCASE("descriptor offset identifies another valid page") {
		if (pages.size() < 2) {
			FAIL("fixture needs two distinct pages for the wrong-offset control");
			return;
		}
		pages[0].offset = pages[1].offset;
		pages[0].stored_bytes = pages[1].stored_bytes;
		expected_reason = "gsq_page_mismatch";
	}
	SUBCASE("decoded digest mismatch") {
		pages[0].decoded_digest[0] ^= 1;
		expected_reason = "gsq_decoded_digest_mismatch";
	}
	Store store;
	if (!install(store, path, pages)) {
		return;
	}
	Store::DecodedPage output;
	output.gaussians.resize(1); // A failed call must clear previous output as well.
	output.sh_high_order.resize(1);
	output.digest_status = Store::DIGEST_CHECKED;
	Error error = OK;
	String reason;
	if (!capture_page(store, pages[0], 1, verify_decoded_digest, output, error, reason)) {
		return;
	}
	CHECK(error != OK);
	CHECK(output.gaussians.is_empty());
	CHECK(output.sh_high_order.is_empty());
	CHECK(output.digest_status == Store::DIGEST_NOT_PERFORMED);
	CHECK(reason.contains(expected_reason));
	const Store::Stats stats = store.stats();
	CHECK(stats.captures == 1);
	CHECK(stats.captures_failed == 1);
	CHECK(stats.page_failures == 1);
	CHECK(stats.pages_verified == 0);
	CHECK(stats.pages_decoded == 0);
	CHECK(stats.splats_decoded == 0);
	CHECK(stats.pages_decoded_digest_checked == 0);
	CHECK(stats.pages_decoded_digest_skipped == 0);
	CHECK(stats.decode_nsec == 0);
	CHECK(stats.verify_nsec == 0);
	CHECK(stats.digest_nsec == 0);
	CHECK(stats.stored_bytes_read == pages[0].stored_bytes);
	CHECK(stats.last_failure_reason.contains(expected_reason));
	CHECK(store.take_thread_last_capture_decode_nsec() == 0);
	check_drained(stats, 1);
	// A validation failure must return its decoder lease, too.
	CHECK(stats.decoder_leases == 1);
	CHECK(stats.decoder_returns == 1);
	CHECK(stats.decoders_leased == 0);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] failure control detects dropped stash ownership transfer") {
	using namespace TestPayloadPageStoreContracts;
	RawFile file("fault_transfer");
	if (!file.valid) {
		FAIL("could not write ownership failure-control fixture");
		return;
	}
	DispatchReport healthy;
	DispatchReport faulty;
	if (!dispatch_contract(file, Store::TEST_FAULT_NONE, healthy) || !dispatch_contract(file, Store::TEST_FAULT_DROP_STASH_TRANSFER, faulty)) {
		return;
	}
	CHECK(healthy.accounting_ok);
	CHECK(healthy.routing_ok);
	CHECK(healthy.faults_triggered == 0);
	CHECK(faulty.faults_triggered > 0); // Removing hook wiring must also turn this red.
	CHECK_FALSE(faulty.accounting_ok);
}

TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] failure control detects swapped requester tickets") {
	using namespace TestPayloadPageStoreContracts;
	RawFile file("fault_tickets");
	if (!file.valid) {
		FAIL("could not write ticket failure-control fixture");
		return;
	}
	DispatchReport healthy;
	DispatchReport faulty;
	if (!dispatch_contract(file, Store::TEST_FAULT_NONE, healthy) || !dispatch_contract(file, Store::TEST_FAULT_SWAP_REQUESTER_TICKETS, faulty)) {
		return;
	}
	CHECK(healthy.accounting_ok);
	CHECK(healthy.routing_ok);
	CHECK(healthy.faults_triggered == 0);
	CHECK(faulty.faults_triggered > 0);
	CHECK_FALSE(faulty.routing_ok);
}
