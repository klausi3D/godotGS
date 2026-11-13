#include "gs_payload_page_store.h"

// PHASE 1 ONLY. These stubs must not admit work, allocate a decoder, read a
// file, fabricate a completion, or report successful work. Phase 2 replaces
// them with the implementation after the contract/identity tests are reviewed.

PayloadPageStore::~PayloadPageStore() = default;
PayloadPageStore::StoredPage::~StoredPage() = default;
PayloadPageStore::DecoderLease::~DecoderLease() = default;

PayloadPageStore::StoredPage::StoredPage(StoredPage &&p_other) noexcept {}

PayloadPageStore::StoredPage &PayloadPageStore::StoredPage::operator=(StoredPage &&p_other) noexcept {
	return *this;
}

bool PayloadPageStore::StoredPage::is_valid() const {
	return false;
}

PayloadPageStore::Identity PayloadPageStore::StoredPage::get_identity() const {
	return Identity();
}

Error PayloadPageStore::StoredPage::get_error() const {
	return ERR_UNAVAILABLE;
}

const Vector<uint8_t> &PayloadPageStore::StoredPage::get_bytes() const {
	return empty_bytes;
}

void PayloadPageStore::StoredPage::reset() {}

PayloadPageStore::DecoderLease::DecoderLease(DecoderLease &&p_other) noexcept {}

PayloadPageStore::DecoderLease &PayloadPageStore::DecoderLease::operator=(DecoderLease &&p_other) noexcept {
	return *this;
}

gs_gsq::DecodeContext *PayloadPageStore::DecoderLease::get() const {
	return nullptr;
}

void PayloadPageStore::DecoderLease::reset() {}

Error PayloadPageStore::start(const Config &p_config) {
	return ERR_UNAVAILABLE;
}

Error PayloadPageStore::replace_source(const String &p_path, uint64_t p_generation, const LocalVector<PageDescriptor> &p_pages) {
	return ERR_UNAVAILABLE;
}

PayloadReadService::SubmitResult PayloadPageStore::request(uint64_t p_requester, uint64_t p_ticket, const PageDescriptor &p_page) {
	return PayloadReadService::SUBMIT_NOT_RUNNING;
}

bool PayloadPageStore::wait(uint64_t p_requester, uint64_t p_ticket, StoredPage &r_page) {
	return false;
}

Error PayloadPageStore::cancel_requester(uint64_t p_requester) {
	return ERR_UNAVAILABLE;
}

void PayloadPageStore::shutdown() {}

PayloadPageStore::Stats PayloadPageStore::stats() const {
	return Stats();
}

Error PayloadPageStore::lease_decoder(DecoderLease &r_lease) {
	return ERR_UNAVAILABLE;
}

Error PayloadPageStore::decode_gsq_page(const StoredPage &p_page, DecoderLease &p_decoder, bool p_verify_decoded_digest,
		DecodedPage &r_page, String &r_reason) {
	r_page.gaussians.clear();
	r_page.sh_high_order.clear();
	r_page.digest_status = DIGEST_NOT_PERFORMED;
	r_reason = "gsq_page_store_unavailable";
	return ERR_UNAVAILABLE;
}

void PayloadPageStore::begin_capture() {}

void PayloadPageStore::end_capture(bool p_success, const String &p_reason) {}

uint64_t PayloadPageStore::take_thread_last_capture_decode_nsec() {
	return 0;
}

#ifdef TESTS_ENABLED
Error PayloadPageStore::_test_set_fault(TestFault p_fault) {
	return ERR_UNAVAILABLE;
}

bool PayloadPageStore::_test_dispatch_one_completion() {
	return false;
}

uint64_t PayloadPageStore::_test_faults_triggered() const {
	return 0;
}
#endif
