#pragma once

#include "test_gs_payload_page_store.h"

// The chunk route uses the service budget alone. Completed stored bytes remain
// visible in accounting but must not prevent a read the service can admit.
TEST_CASE("[GaussianSplatting][WorldIO][PayloadPageStore] legacy admission tracks completed bytes without charging them to the service limit") {
	using namespace TestPayloadPageStoreContracts;
	RawFile file("legacy_admission");
	if (!file.valid) {
		FAIL("could not write the legacy admission fixture");
		return;
	}
	Store store;
	Store::Config config;
	config.reader_count = 1;
	config.max_bytes_in_flight = 160;
	CHECK(config.admission_policy == Store::ADMIT_SERVICE_BYTES_ONLY);
	LocalVector<Store::PageDescriptor> pages = raw_descriptors(4);
	pages[1].stored_bytes = 96;
	pages[3].stored_bytes = 96;
	if (!install(store, file.path, pages, config) || !request(store, 10, 1, pages[0]) || !request(store, 20, 1, pages[1])) {
		return;
	}
	if (!store._test_dispatch_one_completion()) {
		FAIL("must retain a real completion in the dispatch stash");
		return;
	}
	Store::Stats stats = store.stats();
	CHECK(stats.service_bytes == 96);
	CHECK(stats.reads.bytes_in_flight == 96);
	CHECK(stats.stash_bytes == 64);
	CHECK(stats.retained_bytes == 160);
	if (!request(store, 30, 1, pages[2])) {
		return; // Retained-byte admission would incorrectly refuse this legal read.
	}
	stats = store.stats();
	CHECK(stats.service_bytes == 160);
	CHECK(stats.reads.bytes_in_flight == 160);
	CHECK(stats.stash_bytes == 64);
	CHECK(stats.retained_bytes == 224);
	CHECK(stats.peak_retained_bytes == 224);
	CHECK(stats.refused_retained_busy == 0);
	CHECK(stats.reads.refused_busy == 0);
	// Native saturation still refuses the next page and counts exactly one refusal.
	CHECK(store.request(40, 1, pages[3]) == Read::SUBMIT_BUSY);
	CHECK(store.stats().reads.refused_busy == 1);
	CHECK(store.stats().refused_retained_busy == 0);
	Store::StoredPage held;
	if (!take(store, 20, 1, held)) {
		return;
	}
	CHECK(bytes_match(held, file, pages[1], 20, 1));
	stats = store.stats();
	CHECK(stats.service_bytes == 64);
	CHECK(stats.stash_bytes == 64);
	CHECK(stats.caller_bytes == 96);
	CHECK(stats.retained_bytes == 224);
	if (!request(store, 40, 1, pages[3])) {
		return; // Caller-held bytes must not strengthen the legacy service budget.
	}
	stats = store.stats();
	CHECK(stats.service_bytes == 160);
	CHECK(stats.reads.bytes_in_flight == 160);
	CHECK(stats.stash_bytes == 64);
	CHECK(stats.caller_bytes == 96);
	CHECK(stats.retained_bytes == 320);
	CHECK(stats.peak_retained_bytes == 320);
	CHECK(stats.reservations_acquired == 4);
	CHECK(stats.reservations_released == 0);
	const uint64_t requesters[] = { 10, 30, 40 };
	const uint32_t page_indices[] = { 0, 2, 3 };
	for (uint32_t i = 0; i < 3; i++) {
		Store::StoredPage page;
		if (!take(store, requesters[i], 1, page)) {
			return;
		}
		CHECK(bytes_match(page, file, pages[page_indices[i]], requesters[i], 1));
	}
	held.reset();
	stats = store.stats();
	check_drained(stats, 4);
	CHECK(stats.peak_service_bytes == 160);
	CHECK(stats.peak_stash_bytes >= 64);
	CHECK(stats.peak_retained_bytes == 320);
	CHECK(stats.refused_retained_busy == 0);
	CHECK(stats.reads.refused_busy == 1);
	CHECK(stats.reads.submitted == 4);
	CHECK(stats.reads.completed_ok == 4);
	CHECK(stats.reads.completed_failed == 0);
	CHECK(stats.reads.bytes_read == 320);
}
