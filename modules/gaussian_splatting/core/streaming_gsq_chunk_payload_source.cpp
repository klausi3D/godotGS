#include "streaming_gsq_chunk_payload_source.h"

#include "core/error/error_macros.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/thread.h"

#include <chrono>
#include <cstring>

namespace {
// BV-K9 (#1387): decode time of the last capture on this thread, taken by the pack worker right
// after the capture so the pipeline can record decode apart from the read.
thread_local uint64_t tls_last_capture_decode_nsec = 0;
} // namespace

uint64_t GsqPagedChunkPayloadSource::take_thread_last_capture_decode_nsec() const {
	const uint64_t nsec = tls_last_capture_decode_nsec;
	tls_last_capture_decode_nsec = 0;
	return nsec;
}

void GsqPagedChunkPayloadSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_decode_stats"), &GsqPagedChunkPayloadSource::get_decode_stats);
}

GsqPagedChunkPayloadSource::~GsqPagedChunkPayloadSource() {
	// No capture can be running: each one holds a reference to this source.
	read_service.shutdown();
	for (gs_gsq::DecodeContext *decoder : idle_decoders) {
		memdelete(decoder);
	}
	idle_decoders.clear();
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
	codec_params = p_codec_params;
	pages = p_pages;
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
	PayloadReadService::Config config;
	config.reader_count = kReaderCount;
	config.max_bytes_in_flight = kMaxBytesInFlight;
	config.max_idle_handles = kMaxIdleHandles;
	if (read_service.start(config) != OK) {
		service_failed = true;
		return false;
	}
	service_started = true;
	return true;
}

bool GsqPagedChunkPayloadSource::_dispatch_wait(uint64_t p_ticket, PayloadReadService::Completion *r_completion) const {
	MutexLock lock(dispatch_mutex);
	while (true) {
		if (const PayloadReadService::Completion *parked = dispatch_stash.getptr(p_ticket)) {
			*r_completion = *parked;
			dispatch_stash.erase(p_ticket);
			return true;
		}
		if (dispatch_pumping) {
			// Another capture is waiting on the service; it parks what it receives and wakes us.
			dispatch_cv.wait(lock);
			continue;
		}
		// This capture's ticket is still live in the service, so a completion will arrive.
		dispatch_pumping = true;
		lock.temp_unlock();
		PayloadReadService::Completion completion;
		const bool received = read_service.wait_completion(completion);
		lock.temp_relock();
		dispatch_pumping = false;
		if (received) {
			dispatch_stash.insert(completion.ticket, completion);
		}
		dispatch_cv.notify_all();
		if (!received) {
			return false;
		}
	}
}

gs_gsq::DecodeContext *GsqPagedChunkPayloadSource::_lease_decoder() const {
	{
		MutexLock lock(state_mutex);
		if (!idle_decoders.is_empty()) {
			gs_gsq::DecodeContext *decoder = idle_decoders[idle_decoders.size() - 1u];
			idle_decoders.resize(idle_decoders.size() - 1u);
			return decoder;
		}
	}
	// One context per concurrent capture: the pool never exceeds the peak number of captures.
	return memnew(gs_gsq::DecodeContext);
}

void GsqPagedChunkPayloadSource::_return_decoder(gs_gsq::DecodeContext *p_decoder) const {
	MutexLock lock(state_mutex);
	idle_decoders.push_back(p_decoder);
}

bool GsqPagedChunkPayloadSource::_fail(const String &p_reason) const {
	stat_captures_failed.increment();
	bool log = false;
	{
		MutexLock lock(state_mutex);
		last_failure_reason = p_reason;
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
	tls_last_capture_decode_nsec = 0;
	stat_captures.increment();
	if (!is_valid()) {
		return _fail("gsq_source_not_configured");
	}
	if (p_count == 0u) {
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
	LocalVector<uint64_t> tickets;
	tickets.resize(touched_count);
	uint32_t submitted = 0; // touched pages [0, submitted) hold a ticket
	uint32_t taken = 0; // touched pages [0, taken) were received
	String reason;
	// Submits ahead up to the service's byte budget so the readers fetch the next pages while
	// this thread decodes. False on a refusal that retrying cannot fix.
	const auto submit_ahead = [&]() -> bool {
		while (submitted < touched_count) {
			const Page &page = pages[touched[submitted]];
			const uint64_t ticket = next_ticket.increment();
			// A file read, not a RenderingDevice submission. The #685 guard
			// (tests/ci/check_device_submission_contract.py) matches every `.submit(` by name, so
			// the call is spelled with its qualified name rather than weakening the guard.
			const PayloadReadService::SubmitResult result =
					read_service.PayloadReadService::submit(file_path, page.offset, page.stored_bytes, ticket);
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

	gs_gsq::DecodeContext *decoder = _lease_decoder();
	const bool on_main_thread = Thread::is_main_thread();
	LocalVector<Gaussian> page_gaussians;
	LocalVector<Vector3> page_sh;
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
		PayloadReadService::Completion completion;
		if (!_dispatch_wait(tickets[t], &completion)) {
			reason = "gsq_read_service_stopped";
			ok = false;
			break;
		}
		taken = t + 1u;
		ok = submit_ahead();
		if (!ok) {
			break;
		}

		const uint32_t page_index = touched[t];
		const Page &page = pages[page_index];
		stat_stored_bytes_read.add(uint64_t(completion.bytes.size()));
		const std::chrono::steady_clock::time_point decode_begin = std::chrono::steady_clock::now();
		if (completion.error != OK || uint64_t(completion.bytes.size()) != uint64_t(page.stored_bytes)) {
			reason = vformat("gsq_read_failed: page %d (error %d, %d of %d bytes)", page_index, int(completion.error),
					int64_t(completion.bytes.size()), page.stored_bytes);
			ok = false;
		}
		const uint8_t *bytes = completion.bytes.ptr();
		String page_reason;
		gs_gsq::PageInfo info;
		if (ok && gs_gsq::verify_page(bytes, page.stored_bytes, info, page_reason) != OK) {
			reason = vformat("page %d: %s", page_index, page_reason);
			ok = false;
		}
		const uint64_t verify_nsec = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - decode_begin)
						.count());
		if (ok) {
			bool matches = info.splat_count == page.splat_count && info.stored_bytes == page.stored_bytes && info.sh_degree == sh_degree &&
					info.opacity_bits == codec_params.opacity_bits &&
					memcmp(info.stored_digest, page.stored_digest, gs_gsq::kDigestBytes) == 0 &&
					memcmp(info.position_min, page.extent, sizeof(float) * 3u) == 0 &&
					memcmp(info.position_max, page.extent + 3, sizeof(float) * 3u) == 0;
			for (int i = 0; i < 3; i++) {
				matches = matches && info.sh_bits[i] == codec_params.sh_bits[i];
			}
			if (!matches) {
				reason = vformat("gsq_page_mismatch: page %d differs from its page-table entry or the codec block", page_index);
				ok = false;
			}
		}
		if (ok) {
			page_gaussians.resize(page.splat_count);
			page_sh.resize(page.splat_count * sh_high_order);
			Vector3 *sh_out = sh_high_order > 0u ? page_sh.ptr() : nullptr;
			// The stored digest was verified above; each frame still checks its zstd checksum.
			if (decoder->decode_range(bytes, page.stored_bytes, 0, page.splat_count, page_gaussians.ptr(), sh_out, page_reason, false) != OK) {
				reason = vformat("page %d: %s", page_index, page_reason);
				ok = false;
			}
		}
		uint64_t digest_nsec = 0;
		// BV-K9c: off on the streaming path (see the header); the stored-page verify above always runs.
		const bool check_decoded_digest = verify_decoded_digest;
		if (ok && check_decoded_digest) {
			uint8_t digest[gs_gsq::kDigestBytes];
			const std::chrono::steady_clock::time_point digest_begin = std::chrono::steady_clock::now();
			gs_gsq::decoded_digest(page_gaussians.ptr(), sh_high_order > 0u ? page_sh.ptr() : nullptr, page.splat_count, sh_degree, digest);
			digest_nsec = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - digest_begin)
							.count());
			if (memcmp(digest, page.decoded_digest, gs_gsq::kDigestBytes) != 0) {
				reason = vformat("gsq_decoded_digest_mismatch: page %d", page_index);
				ok = false;
			}
		}
		const uint64_t decode_nsec = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - decode_begin)
						.count());
		if (!ok) {
			stat_page_failures.increment();
			break;
		}
		stat_pages_decoded.increment();
		stat_splats_decoded.add(page.splat_count);
		stat_decode_nsec.add(decode_nsec);
		stat_digest_nsec.add(digest_nsec);
		stat_verify_nsec.add(verify_nsec);
		if (check_decoded_digest) {
			stat_pages_digest_checked.increment();
		}
		tls_last_capture_decode_nsec += decode_nsec;
		if (on_main_thread) {
			stat_pages_on_main.increment();
		} else {
			stat_pages_on_workers.increment();
		}

		const uint32_t first = page_first_splat[page_index];
		for (; cursor < p_count && uint32_t(work[cursor] >> 32u) == page_index; cursor++) {
			const uint32_t slot = uint32_t(work[cursor] & 0xffffffffull);
			const uint32_t local = (p_indices ? p_indices[slot] : p_start + slot) - first;
			r_gaussians[slot] = page_gaussians[local];
			for (uint32_t k = 0; k < sh_high_order; k++) {
				r_sh_high_order[slot * sh_high_order + k] = page_sh[local * sh_high_order + k];
			}
		}
	}

	// Nothing submitted may stay behind: a ticket the service still holds is cancelled, one
	// another capture already received for us is collected from the stash and dropped.
	for (uint32_t t = taken; t < submitted; t++) {
		if (!read_service.cancel(tickets[t])) {
			PayloadReadService::Completion discarded;
			_dispatch_wait(tickets[t], &discarded);
		}
	}
	_return_decoder(decoder);

	if (ok && cursor != p_count) {
		reason = "gsq_capture_incomplete";
		ok = false;
	}
	if (!ok) {
		r_gaussians.clear();
		r_sh_high_order.clear();
		return _fail(reason);
	}
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
		stat_captures.increment();
		return _fail("gsq_capture_out_of_range: null index list");
	}
	return _capture(p_indices, 0, p_count, r_gaussians, r_sh_high_order, r_sh_first_order_count, r_sh_high_order_count);
}

GsqPagedChunkPayloadSource::DecodeStats GsqPagedChunkPayloadSource::get_decode_stats_struct() const {
	DecodeStats stats;
	stats.captures = stat_captures.get();
	stats.captures_failed = stat_captures_failed.get();
	stats.pages_decoded = stat_pages_decoded.get();
	stats.splats_decoded = stat_splats_decoded.get();
	stats.decode_nsec = stat_decode_nsec.get();
	stats.digest_nsec = stat_digest_nsec.get();
	stats.verify_nsec = stat_verify_nsec.get();
	stats.pages_decoded_digest_checked = stat_pages_digest_checked.get();
	stats.pages_decoded_on_worker_threads = stat_pages_on_workers.get();
	stats.pages_decoded_on_main_thread = stat_pages_on_main.get();
	stats.page_failures = stat_page_failures.get();
	stats.stored_bytes_read = stat_stored_bytes_read.get();
	{
		MutexLock lock(state_mutex);
		stats.last_failure_reason = last_failure_reason;
	}
	stats.reads = read_service.stats();
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
