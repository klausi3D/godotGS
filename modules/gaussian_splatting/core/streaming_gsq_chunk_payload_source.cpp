#include "streaming_gsq_chunk_payload_source.h"

#include "core/error/error_macros.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

#include <cstring>

uint64_t GsqPagedChunkPayloadSource::take_thread_last_capture_decode_nsec() const {
	return page_store.take_thread_last_capture_decode_nsec();
}

void GsqPagedChunkPayloadSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_decode_stats"), &GsqPagedChunkPayloadSource::get_decode_stats);
}

GsqPagedChunkPayloadSource::~GsqPagedChunkPayloadSource() {
	// No capture can be running: each one holds a reference to this source.
	page_store.shutdown();
}

bool GsqPagedChunkPayloadSource::configure(const String &p_path, uint32_t p_splat_count, uint32_t p_sh_degree,
		uint32_t p_sh_first_order, uint32_t p_sh_high_order, const AABB &p_bounds, const gs_gsq::EncodeParams &p_codec_params,
		const LocalVector<Page> &p_pages) {
	if (p_path.is_empty() || p_splat_count == 0u || p_pages.is_empty() || p_sh_degree > gs_gsq::kMaxShDegree ||
			p_sh_high_order != gs_gsq::sh_high_order_for_degree(p_sh_degree)) {
		return false;
	}
	LocalVector<uint32_t> firsts;
	firsts.resize(p_pages.size() + 1u);
	LocalVector<PayloadPageStore::PageDescriptor> descriptors;
	descriptors.resize(p_pages.size());
	uint64_t total = 0;
	for (uint32_t p = 0; p < p_pages.size(); p++) {
		if (p_pages[p].splat_count == 0u || p_pages[p].splat_count > gs_gsq::kMaxPageSplats ||
				p_pages[p].stored_bytes < gs_gsq::kPageHeaderBytes) {
			return false;
		}
		firsts[p] = uint32_t(total);
		total += p_pages[p].splat_count;
		if (total > uint64_t(p_splat_count)) {
			return false;
		}
		PayloadPageStore::PageDescriptor &descriptor = descriptors[p];
		descriptor.id = p;
		descriptor.offset = p_pages[p].offset;
		descriptor.stored_bytes = p_pages[p].stored_bytes;
		descriptor.encoding = PayloadPageStore::GSQ_V1;
		descriptor.splat_count = p_pages[p].splat_count;
		descriptor.sh_degree = p_sh_degree;
		descriptor.codec_params = p_codec_params;
		memcpy(descriptor.stored_digest, p_pages[p].stored_digest, sizeof(descriptor.stored_digest));
		memcpy(descriptor.decoded_digest, p_pages[p].decoded_digest, sizeof(descriptor.decoded_digest));
		memcpy(descriptor.extent, p_pages[p].extent, sizeof(descriptor.extent));
	}
	if (total != uint64_t(p_splat_count)) {
		return false;
	}
	firsts[p_pages.size()] = p_splat_count;

	file_path = p_path;
	splat_count = p_splat_count;
	sh_degree = p_sh_degree;
	sh_first_order = p_sh_first_order;
	sh_high_order = p_sh_high_order;
	bounds = p_bounds;
	pages = descriptors;
	page_first_splat = firsts;
	return true;
}

uint32_t GsqPagedChunkPayloadSource::_page_of(uint32_t p_splat) const {
	// page_first_splat[lo] <= p_splat < page_first_splat[hi]
	uint32_t lo = 0;
	uint32_t hi = pages.size();
	while (hi - lo > 1u) {
		const uint32_t mid = lo + (hi - lo) / 2u;
		if (page_first_splat[mid] <= p_splat) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return lo;
}

bool GsqPagedChunkPayloadSource::_ensure_service_started() const {
	MutexLock lock(state_mutex);
	if (service_started) {
		return true;
	}
	if (service_failed) {
		return false;
	}
	PayloadPageStore::Config config;
	config.reader_count = kReaderCount;
	config.max_bytes_in_flight = kMaxBytesInFlight;
	config.max_idle_handles = kMaxIdleHandles;
	config.admission_policy = PayloadPageStore::ADMIT_SERVICE_BYTES_ONLY;
	if (page_store.start(config) != OK || page_store.replace_source(file_path, 1u, pages) != OK) {
		page_store.shutdown();
		service_failed = true;
		return false;
	}
	service_started = true;
	return true;
}

bool GsqPagedChunkPayloadSource::_fail(const String &p_reason) const {
	page_store.end_capture(false, p_reason);
	bool log = false;
	{
		MutexLock lock(state_mutex);
		log = !failure_logged;
		failure_logged = true;
	}
	if (log) {
		// Once per source; every failure is counted in captures_failed and page_failures.
		ERR_PRINT(vformat("[GaussianSplatWorld] Paged-GSQ capture failed for %s: %s (further failures are counted, not logged)",
				file_path, p_reason));
	}
	return false;
}

bool GsqPagedChunkPayloadSource::_capture(const uint32_t *p_indices, uint32_t p_start, uint32_t p_count,
		LocalVector<Gaussian> &r_gaussians, LocalVector<Vector3> &r_sh_high_order,
		uint32_t &r_sh_first_order_count, uint32_t &r_sh_high_order_count) const {
	r_gaussians.clear();
	r_sh_high_order.clear();
	r_sh_first_order_count = sh_first_order;
	r_sh_high_order_count = sh_high_order;
	page_store.begin_capture();
	if (!is_valid()) {
		return _fail("gsq_source_not_configured");
	}
	if (p_count == 0u) {
		page_store.end_capture(true, String());
		return true;
	}
	if (p_indices == nullptr && (p_start >= splat_count || p_count > splat_count - p_start)) {
		return _fail(vformat("gsq_capture_out_of_range: [%d, %d + %d) of %d splats", p_start, p_start, p_count, splat_count));
	}
	const uint64_t sh_total = uint64_t(p_count) * uint64_t(sh_high_order);
	if (sh_total > uint64_t(UINT32_MAX)) {
		return _fail("gsq_capture_out_of_range: the SH extent exceeds the 32-bit container capacity");
	}

	// Work list in page order: (page << 32) | output slot. A contiguous range is already in
	// page order; an index list is sorted so each touched page is read and decoded once.
	LocalVector<uint64_t> work;
	work.resize(p_count);
	for (uint32_t slot = 0; slot < p_count; slot++) {
		const uint32_t index = p_indices ? p_indices[slot] : p_start + slot;
		if (index >= splat_count) {
			return _fail(vformat("gsq_capture_out_of_range: splat %d of %d", index, splat_count));
		}
		work[slot] = (uint64_t(_page_of(index)) << 32u) | uint64_t(slot);
	}
	if (p_indices) {
		work.sort();
	}
	LocalVector<uint32_t> touched;
	for (uint64_t entry : work) {
		const uint32_t page = uint32_t(entry >> 32u);
		if (touched.is_empty() || touched[touched.size() - 1u] != page) {
			touched.push_back(page);
		}
	}

	if (!_ensure_service_started()) {
		return _fail("gsq_read_service_unavailable");
	}
	r_gaussians.resize(p_count);
	r_sh_high_order.resize(uint32_t(sh_total));

	const uint32_t touched_count = touched.size();
	const uint64_t requester = next_requester.increment();
	LocalVector<uint64_t> tickets;
	tickets.resize(touched_count);
	uint32_t submitted = 0; // touched pages [0, submitted) hold a ticket
	String reason;
	// Submits ahead up to the service's byte budget so the readers fetch the next pages while
	// this thread decodes. False on a refusal that retrying cannot fix.
	const auto submit_ahead = [&]() -> bool {
		while (submitted < touched_count) {
			const uint64_t ticket = uint64_t(submitted) + 1u;
			const PayloadReadService::SubmitResult result =
					page_store.request(requester, ticket, pages[touched[submitted]]);
			if (result == PayloadReadService::SUBMIT_OK) {
				tickets[submitted++] = ticket;
				continue;
			}
			if (result == PayloadReadService::SUBMIT_BUSY) {
				return true;
			}
			reason = vformat("gsq_read_refused: page %d (submit result %d)", touched[submitted], int(result));
			return false;
		}
		return true;
	};

	PayloadPageStore::DecoderLease decoder;
	if (page_store.lease_decoder(decoder) != OK) {
		r_gaussians.clear();
		r_sh_high_order.clear();
		return _fail("gsq_decoder_unavailable");
	}
	PayloadPageStore::DecodedPage decoded;
	bool ok = true;
	uint32_t cursor = 0; // into work
	for (uint32_t t = 0; ok && t < touched_count; t++) {
		while (ok && submitted <= t) {
			ok = submit_ahead();
			if (ok && submitted <= t) {
				// The byte budget is held by other captures' reads; they complete without us.
				OS::get_singleton()->delay_usec(100);
			}
		}
		if (!ok) {
			break;
		}
		PayloadPageStore::StoredPage completion;
		if (!page_store.wait(requester, tickets[t], completion)) {
			reason = "gsq_read_service_stopped";
			ok = false;
			break;
		}
		ok = submit_ahead();
		if (!ok) {
			break;
		}

		const uint32_t page_index = touched[t];
		if (page_store.decode_gsq_page(completion, decoder, verify_decoded_digest, decoded, reason) != OK) {
			ok = false;
			break;
		}

		const uint32_t first = page_first_splat[page_index];
		for (; cursor < p_count && uint32_t(work[cursor] >> 32u) == page_index; cursor++) {
			const uint32_t slot = uint32_t(work[cursor] & 0xffffffffull);
			const uint32_t local = (p_indices ? p_indices[slot] : p_start + slot) - first;
			r_gaussians[slot] = decoded.gaussians[local];
			for (uint32_t k = 0; k < sh_high_order; k++) {
				r_sh_high_order[slot * sh_high_order + k] = decoded.sh_high_order[local * sh_high_order + k];
			}
		}
	}

	// Drain all remaining service and stash owners, including failures before decode.
	page_store.cancel_requester(requester);

	if (ok && cursor != p_count) {
		reason = "gsq_capture_incomplete";
		ok = false;
	}
	if (!ok) {
		r_gaussians.clear();
		r_sh_high_order.clear();
		return _fail(reason);
	}
	page_store.end_capture(true, String());
	return true;
}

bool GsqPagedChunkPayloadSource::capture_chunk_snapshot(uint32_t p_start, uint32_t p_count,
		LocalVector<Gaussian> &r_gaussians,
		LocalVector<Vector3> &r_sh_high_order,
		uint32_t &r_sh_first_order_count,
		uint32_t &r_sh_high_order_count) const {
	return _capture(nullptr, p_start, p_count, r_gaussians, r_sh_high_order, r_sh_first_order_count, r_sh_high_order_count);
}

bool GsqPagedChunkPayloadSource::capture_indexed_chunk_snapshot(const uint32_t *p_indices, uint32_t p_count,
		LocalVector<Gaussian> &r_gaussians,
		LocalVector<Vector3> &r_sh_high_order,
		uint32_t &r_sh_first_order_count,
		uint32_t &r_sh_high_order_count) const {
	if (p_indices == nullptr && p_count > 0u) {
		r_gaussians.clear();
		r_sh_high_order.clear();
		page_store.begin_capture(false);
		return _fail("gsq_capture_out_of_range: null index list");
	}
	return _capture(p_indices, 0, p_count, r_gaussians, r_sh_high_order, r_sh_first_order_count, r_sh_high_order_count);
}

GsqPagedChunkPayloadSource::DecodeStats GsqPagedChunkPayloadSource::get_decode_stats_struct() const {
	const PayloadPageStore::Stats store_stats = page_store.stats();
	DecodeStats stats;
	stats.captures = store_stats.captures;
	stats.captures_failed = store_stats.captures_failed;
	stats.pages_decoded = store_stats.pages_decoded;
	stats.splats_decoded = store_stats.splats_decoded;
	stats.decode_nsec = store_stats.decode_nsec;
	stats.digest_nsec = store_stats.digest_nsec;
	stats.verify_nsec = store_stats.verify_nsec;
	stats.pages_decoded_digest_checked = store_stats.pages_decoded_digest_checked;
	stats.pages_decoded_on_worker_threads = store_stats.pages_decoded_on_worker_threads;
	stats.pages_decoded_on_main_thread = store_stats.pages_decoded_on_main_thread;
	stats.page_failures = store_stats.page_failures;
	stats.stored_bytes_read = store_stats.stored_bytes_read;
	stats.last_failure_reason = store_stats.last_failure_reason;
	stats.reads = store_stats.reads;
	return stats;
}

Dictionary GsqPagedChunkPayloadSource::get_decode_stats() const {
	const DecodeStats stats = get_decode_stats_struct();
	Dictionary d;
	d["page_count"] = int64_t(pages.size());
	d["captures"] = int64_t(stats.captures);
	d["captures_failed"] = int64_t(stats.captures_failed);
	d["pages_decoded"] = int64_t(stats.pages_decoded);
	d["splats_decoded"] = int64_t(stats.splats_decoded);
	d["decode_nsec"] = int64_t(stats.decode_nsec);
	d["digest_nsec"] = int64_t(stats.digest_nsec);
	d["verify_nsec"] = int64_t(stats.verify_nsec);
	d["decoded_digest_verified"] = verify_decoded_digest;
	d["pages_decoded_digest_checked"] = int64_t(stats.pages_decoded_digest_checked);
	d["pages_decoded_on_worker_threads"] = int64_t(stats.pages_decoded_on_worker_threads);
	d["pages_decoded_on_main_thread"] = int64_t(stats.pages_decoded_on_main_thread);
	d["page_failures"] = int64_t(stats.page_failures);
	d["stored_bytes_read"] = int64_t(stats.stored_bytes_read);
	d["last_failure_reason"] = stats.last_failure_reason;
	d["reads_submitted"] = int64_t(stats.reads.submitted);
	d["reads_completed_ok"] = int64_t(stats.reads.completed_ok);
	d["reads_completed_failed"] = int64_t(stats.reads.completed_failed);
	d["reads_refused_busy"] = int64_t(stats.reads.refused_busy);
	d["read_bytes"] = int64_t(stats.reads.bytes_read);
	d["read_handle_opens"] = int64_t(stats.reads.handle_opens);
	return d;
}
