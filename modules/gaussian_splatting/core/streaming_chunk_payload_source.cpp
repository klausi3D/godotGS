#include "streaming_chunk_payload_source.h"

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "../logger/gs_logger.h"

namespace {

constexpr uint32_t INDEXED_CONTIGUOUS_READ_MAX_OVERREAD_FACTOR = 2;

struct IndexedReadRequest {
	uint32_t index = 0;
	uint32_t output = 0;
};

struct IndexedReadRequestComparator {
	_FORCE_INLINE_ bool operator()(const IndexedReadRequest &p_a, const IndexedReadRequest &p_b) const {
		return p_a.index == p_b.index ? p_a.output < p_b.output : p_a.index < p_b.index;
	}
};

_FORCE_INLINE_ bool _indexed_request_extends_run(uint32_t p_index, uint32_t p_run_end) {
	return p_index == p_run_end || (p_run_end < UINT32_MAX && p_index == p_run_end + 1);
}

// Fail-closed guard for the staged SH capture. The destination is a
// uint32_t-indexed LocalVector<Vector3>, so an element count above UINT32_MAX
// cannot be represented. Historically the resize truncated the 64-bit count to
// 32 bits (`resize(uint32_t(count))`) while the copy used the full 64-bit width,
// overrunning the heap when `p_count * sh_high_order > UINT32_MAX` (reachable via
// a hostile sparse `.gsplatworld`; the indexed path can also cross the boundary
// through duplicate indices, where the requested slot count exceeds splat_count).
// Refuse rather than truncate. On success `r_count` receives the u32-safe count.
_FORCE_INLINE_ bool _sh_capture_count_fits_u32(uint64_t p_element_count, const char *p_label, uint32_t &r_count) {
	if (p_element_count > uint64_t(UINT32_MAX)) {
		ERR_PRINT(vformat("[StagedFileSource] Refusing %s SH capture: %s coefficients exceed the 32-bit destination capacity.",
				p_label, String::num_uint64(p_element_count)));
		return false;
	}
	r_count = uint32_t(p_element_count);
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// ChunkPayloadSource
// ---------------------------------------------------------------------------

void ChunkPayloadSource::_bind_methods() {}

// ---------------------------------------------------------------------------
// InMemoryChunkPayloadSource
// ---------------------------------------------------------------------------

void InMemoryChunkPayloadSource::_bind_methods() {}

bool InMemoryChunkPayloadSource::capture_chunk_snapshot(uint32_t p_start, uint32_t p_count,
		LocalVector<Gaussian> &r_gaussians,
		LocalVector<Vector3> &r_sh_high_order,
		uint32_t &r_sh_first_order_count,
		uint32_t &r_sh_high_order_count) const {
	if (!data.is_valid()) {
		return false;
	}
	return data->capture_chunk_snapshot(p_start, p_count,
			r_gaussians, r_sh_high_order, r_sh_first_order_count, r_sh_high_order_count);
}

bool InMemoryChunkPayloadSource::capture_indexed_chunk_snapshot(const uint32_t *p_indices, uint32_t p_count,
		LocalVector<Gaussian> &r_gaussians,
		LocalVector<Vector3> &r_sh_high_order,
		uint32_t &r_sh_first_order_count,
		uint32_t &r_sh_high_order_count) const {
	if (!data.is_valid()) {
		return false;
	}
	return data->capture_indexed_chunk_snapshot(p_indices, p_count,
			r_gaussians, r_sh_high_order, r_sh_first_order_count, r_sh_high_order_count);
}

uint32_t InMemoryChunkPayloadSource::get_count() const {
	return data.is_valid() ? data->get_count() : 0;
}

uint32_t InMemoryChunkPayloadSource::get_sh_degree() const {
	return data.is_valid() ? data->get_sh_degree() : 0;
}

AABB InMemoryChunkPayloadSource::get_bounds() const {
	return data.is_valid() ? data->get_aabb() : AABB();
}

bool InMemoryChunkPayloadSource::is_valid() const {
	return data.is_valid() && data->get_count() > 0;
}

// ---------------------------------------------------------------------------
// StagedFileChunkPayloadSource
// ---------------------------------------------------------------------------

void StagedFileChunkPayloadSource::_bind_methods() {}

// #714: function-local statics so the registry is initialized on first use and
// cannot depend on translation-unit static init order.
Mutex &StagedFileChunkPayloadSource::_registry_mutex() {
	static Mutex mutex;
	return mutex;
}

HashMap<StagedFileChunkPayloadSource *, bool> &StagedFileChunkPayloadSource::_live_sources() {
	static HashMap<StagedFileChunkPayloadSource *, bool> sources;
	return sources;
}

StagedFileChunkPayloadSource::StagedFileChunkPayloadSource() {
	MutexLock lock(_registry_mutex());
	_live_sources().insert(this, true);
}

StagedFileChunkPayloadSource::~StagedFileChunkPayloadSource() {
	// Unregistering under the registry mutex is what makes
	// release_cached_handles_for_path() safe: it holds that mutex while touching
	// each instance, so an instance cannot be destroyed underneath it.
	MutexLock lock(_registry_mutex());
	_live_sources().erase(this);
}

// #714: how long ScopedReaderSuspend waits for in-flight reads to release their
// FileAccess handles before giving up and letting the caller's rename fail loudly.
static constexpr uint32_t MAX_READER_DRAIN_USEC = 500 * 1000; // 500 ms
static constexpr uint32_t READER_DRAIN_POLL_USEC = 250;
// How long a reader waits for an in-progress replace before opening anyway. Longer
// than the drain budget so the common case resolves by waiting. On timeout the read
// proceeds: a stalled editor import is a better outcome than breaking a running
// scene's streaming, and the import then fails loudly instead of degrading silently.
static constexpr uint32_t MAX_OPEN_SUSPEND_WAIT_USEC = 1000 * 1000; // 1 s

StagedFileChunkPayloadSource::ScopedReaderSuspend::ScopedReaderSuspend(const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}

#if !defined(WINDOWS_ENABLED)
	// #714: this guard exists for one platform-specific reason -- Windows denies the
	// delete access a replace needs while any FileAccess handle is open, because
	// FileAccessWindows opens via _wfsopen with _SH_DENY* and none of those share
	// delete. POSIX rename() has no such constraint: it atomically replaces the
	// destination and open readers simply keep reading the old inode until they close.
	//
	// So on POSIX there is nothing to suspend and nothing to wait for. Doing it anyway
	// would be worse than pointless -- it would spend up to MAX_READER_DRAIN_USEC per
	// reimport and, on timeout, report a not-drained state that makes
	// gs_atomic_file_write() refuse a replace that would have succeeded. That is a
	// regression on Linux/macOS relative to the pre-#714 behaviour, so the guard is a
	// no-op here and reports drained (readers_drained defaults true, nothing is locked,
	// and the destructor's registry_locked/held checks make it inert).
	return;
#endif

	// Handles taken over from the caches, held only until in-flight readers drain.
	// Declared after the platform early-out so POSIX builds do not carry an unused local.
	LocalVector<Ref<FileAccess>> pending;

	// Compare globalized paths: a source configured with "res://x.gsplatworld" and a
	// writer targeting the same file by absolute path must still match.
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const String target = settings ? settings->globalize_path(p_path) : p_path;

	// Held for this object's lifetime (released in the destructor), which is why
	// these are raw lock() calls rather than MutexLock scopes.
	_registry_mutex().lock();
	registry_locked = true;
	for (const KeyValue<StagedFileChunkPayloadSource *, bool> &entry : _live_sources()) {
		StagedFileChunkPayloadSource *source = entry.key;
		// file_mutex is taken only for this short critical section and released
		// immediately. Holding it across the drain below would block an in-flight
		// read's _record_io_counters(), so its Ref would never drop and the wait
		// could never succeed.
		source->file_mutex.lock();
		const String source_path = settings ? settings->globalize_path(source->file_path) : source->file_path;
		if (source_path != target) {
			source->file_mutex.unlock();
			continue;
		}
		// Stop NEW opens (lock-free, so readers never block on us), then take over the
		// cached handles so the only remaining owners are in-flight reads.
		source->suspend_opens.set();
		for (const KeyValue<Thread::ID, Ref<FileAccess>> &cached : source->cached_files) {
			if (cached.value.is_valid()) {
				pending.push_back(cached.value);
			}
		}
		source->cached_files.clear();
		source->file_mutex.unlock();
		held.push_back(source);
	}

	// suspend_opens stops new opens, but a read already past _get_thread_file() holds
	// its own Ref and reads OUTSIDE the mutex, so its OS handle is still open and
	// still denies delete access. Wait for those to drain: once a reader returns, its
	// Ref drops and -- since the cache no longer holds one -- `pending` is the only
	// remaining owner, i.e. reference count 1.
	//
	// file_mutex is NOT held here, by design: the reader must be able to take it to
	// finish (_record_io_counters), or it could never release the Ref being waited on.
	//
	// Bounded on purpose: a read that never returns would otherwise block an import
	// forever. When the budget expires with a handle still in flight we do NOT press
	// on -- `readers_drained` stays false and gs_atomic_file_write() refuses the
	// replace, because that handle still denies delete access and the rename it would
	// attempt is one whose precondition is violated. Reporting the real cause beats
	// reporting whatever DirAccess says about a rename that was never going to work.
	//
	// This is what makes the budget non-load-bearing for correctness: it decides how
	// long an editor import may stall before failing, never whether the file that
	// ends up on disk is correct.
	// The in-flight check runs at least once and always decides `readers_drained`,
	// including when there is no OS singleton to sleep on -- reporting "drained"
	// because we could not wait would be exactly the false clear this guard exists
	// to prevent.
	OS *os = OS::get_singleton();
	uint32_t waited_usec = 0;
	bool any_in_flight = true;
	while (true) {
		any_in_flight = false;
		for (uint32_t i = 0; i < pending.size(); i++) {
			if (pending[i]->get_reference_count() > 1) {
				any_in_flight = true;
				break;
			}
		}
		if (!any_in_flight || os == nullptr || waited_usec >= MAX_READER_DRAIN_USEC) {
			break;
		}
		os->delay_usec(READER_DRAIN_POLL_USEC);
		waited_usec += READER_DRAIN_POLL_USEC;
	}
	readers_drained = !any_in_flight;

	// Dropping our references closes the handles we own. Any handle still counted
	// above belongs to an in-flight read and stays open regardless.
	pending.clear();
}

StagedFileChunkPayloadSource::ScopedReaderSuspend::~ScopedReaderSuspend() {
	// Re-admit opens. No file_mutex needed: suspend_opens is lock-free precisely so
	// this cannot contend with a reader that is mid-read.
	for (uint32_t i = 0; i < held.size(); i++) {
		held[i]->suspend_opens.clear();
	}
	held.clear();
	// Only unlock the registry if the constructor locked it (empty path = no-op).
	if (registry_locked) {
		registry_locked = false;
		_registry_mutex().unlock();
	}
}

void StagedFileChunkPayloadSource::configure(const String &p_path,
		uint64_t p_gaussian_offset,
		uint64_t p_sh_offset,
		uint32_t p_splat_count,
		uint32_t p_sh_degree,
		uint32_t p_sh_first_order,
		uint32_t p_sh_high_order,
		const AABB &p_bounds) {
	MutexLock lock(file_mutex);
	file_path = p_path;
	gaussian_data_offset = p_gaussian_offset;
	sh_data_offset = p_sh_offset;
	splat_count = p_splat_count;
	sh_degree = p_sh_degree;
	sh_first_order = p_sh_first_order;
	sh_high_order = p_sh_high_order;
	bounds = p_bounds;
	cached_files.clear();
	bytes_requested = 0;
	bytes_read = 0;
	file_open_count = 0;
}

Ref<FileAccess> StagedFileChunkPayloadSource::_get_thread_file() const {
	// #714: a writer is atomically replacing this file. Opening a handle now would
	// deny it delete access on Windows and fail the replace, so wait for the guard to
	// finish rather than fail the read outright.
	//
	// The WAIT never holds file_mutex -- a reader that already has a handle needs that
	// mutex to finish (_record_io_counters), and stalling it here would be the very
	// deadlock this lock-free flag exists to avoid. The flag is then RECHECKED once
	// the mutex is held, because a lock-free check followed by a lock is a
	// check-then-act race: the guard can take over the cache in between, and a handle
	// opened after that point would be invisible to it.
	//
	// On exhausting the wait budget the read proceeds anyway: stalling an editor
	// import beats breaking a running scene's streaming, and the import then fails
	// loudly rather than degrading silently.
	OS *os = OS::get_singleton();
	const Thread::ID thread_id = Thread::get_caller_id();
	uint32_t waited_usec = 0;

	while (true) {
		// Wait with NO mutex held: a reader that already holds a handle needs
		// file_mutex to finish (_record_io_counters), and stalling it here would be
		// the deadlock this flag exists to avoid.
		while (suspend_opens.is_set() && os != nullptr && waited_usec < MAX_OPEN_SUSPEND_WAIT_USEC) {
			os->delay_usec(READER_DRAIN_POLL_USEC);
			waited_usec += READER_DRAIN_POLL_USEC;
		}

		MutexLock lock(file_mutex);

		// Recheck UNDER the mutex. The check above is lock-free, so the guard can set
		// the flag and take over the cache in the window between it and this
		// acquisition; opening here would cache a fresh delete-denying handle that the
		// guard never saw, and its rename would fail. Rechecking closes that window:
		// either we get here before the guard (it then takes our handle over with the
		// rest of the cache) or we see the flag and go back to waiting.
		if (suspend_opens.is_set() && os != nullptr && waited_usec < MAX_OPEN_SUSPEND_WAIT_USEC) {
			continue; // releases file_mutex, resumes the wait above
		}

		Ref<FileAccess> *cached_file = cached_files.getptr(thread_id);
		if (cached_file && cached_file->is_valid()) {
			return *cached_file;
		}

		Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::READ);
		if (file.is_null()) {
			ERR_PRINT(vformat("[StagedFileSource] Cannot open staged world file: %s", file_path));
			return Ref<FileAccess>();
		}
		cached_files.insert(thread_id, file);
		file_open_count++;
		return file;
	}
}

bool StagedFileChunkPayloadSource::_read_exact(FileAccess *p_file, uint64_t p_offset, void *p_dst, uint64_t p_bytes, const char *p_label, uint64_t *r_bytes_read) const {
	if (p_bytes == 0) {
		if (r_bytes_read) {
			*r_bytes_read = 0;
		}
		return true;
	}

	p_file->seek(p_offset);
	const uint64_t got = p_file->get_buffer(reinterpret_cast<uint8_t *>(p_dst), p_bytes);
	if (r_bytes_read) {
		*r_bytes_read = got;
	}
	if (got != p_bytes) {
		ERR_PRINT(vformat("[StagedFileSource] Short read on %s: expected %d got %d",
				p_label, p_bytes, got));
		return false;
	}
	return true;
}

void StagedFileChunkPayloadSource::_record_io_counters(uint64_t p_bytes_requested, uint64_t p_bytes_read) const {
	MutexLock lock(file_mutex);
	bytes_requested += p_bytes_requested;
	bytes_read += p_bytes_read;
}

uint64_t StagedFileChunkPayloadSource::get_bytes_requested() const {
	MutexLock lock(file_mutex);
	return bytes_requested;
}

uint64_t StagedFileChunkPayloadSource::get_bytes_read() const {
	MutexLock lock(file_mutex);
	return bytes_read;
}

uint64_t StagedFileChunkPayloadSource::get_file_open_count() const {
	MutexLock lock(file_mutex);
	return file_open_count;
}

void StagedFileChunkPayloadSource::reset_io_counters() {
	MutexLock lock(file_mutex);
	bytes_requested = 0;
	bytes_read = 0;
	file_open_count = 0;
}

bool StagedFileChunkPayloadSource::capture_chunk_snapshot(uint32_t p_start, uint32_t p_count,
		LocalVector<Gaussian> &r_gaussians,
		LocalVector<Vector3> &r_sh_high_order_out,
		uint32_t &r_sh_first_order_count,
		uint32_t &r_sh_high_order_count) const {
	if (file_path.is_empty() || p_count == 0) {
		return false;
	}
	if (uint64_t(p_start) + uint64_t(p_count) > uint64_t(splat_count)) {
		ERR_PRINT(vformat("[StagedFileSource] Range out of bounds: start=%d count=%d total=%d",
				p_start, p_count, splat_count));
		return false;
	}

	Ref<FileAccess> file = _get_thread_file();
	if (file.is_null()) {
		return false;
	}

	// Read gaussian data.
	const uint64_t gaussian_byte_offset = gaussian_data_offset + uint64_t(p_start) * sizeof(Gaussian);
	const uint64_t gaussian_byte_count = uint64_t(p_count) * sizeof(Gaussian);

	r_gaussians.resize(p_count);
	uint64_t physical_bytes_read = 0;
	uint64_t got = 0;
	if (!_read_exact(file.ptr(), gaussian_byte_offset, r_gaussians.ptr(), gaussian_byte_count, "gaussians", &got)) {
		return false;
	}
	physical_bytes_read += got;
	uint64_t logical_bytes_requested = gaussian_byte_count;

	// Read SH high-order coefficients if present.
	r_sh_first_order_count = sh_first_order;
	r_sh_high_order_count = sh_high_order;

	if (sh_high_order > 0 && sh_data_offset > 0) {
		const uint64_t sh_per_splat = uint64_t(sh_high_order);
		const uint64_t sh_element_count = uint64_t(p_count) * sh_per_splat;
		uint32_t sh_element_count_u32 = 0;
		if (!_sh_capture_count_fits_u32(sh_element_count, "contiguous", sh_element_count_u32)) {
			return false;
		}
		const uint64_t sh_byte_offset = sh_data_offset + uint64_t(p_start) * sh_per_splat * sizeof(Vector3);
		// Resize width and copy width both derive from the same u32-verified count.
		const uint64_t sh_byte_count = uint64_t(sh_element_count_u32) * sizeof(Vector3);

		r_sh_high_order_out.resize(sh_element_count_u32);
		if (!_read_exact(file.ptr(), sh_byte_offset, r_sh_high_order_out.ptr(), sh_byte_count, "SH data", &got)) {
			return false;
		}
		logical_bytes_requested += sh_byte_count;
		physical_bytes_read += got;
	} else {
		r_sh_high_order_out.clear();
	}

	_record_io_counters(logical_bytes_requested, physical_bytes_read);
	return true;
}

bool StagedFileChunkPayloadSource::capture_indexed_chunk_snapshot(const uint32_t *p_indices, uint32_t p_count,
		LocalVector<Gaussian> &r_gaussians,
		LocalVector<Vector3> &r_sh_high_order_out,
		uint32_t &r_sh_first_order_count,
		uint32_t &r_sh_high_order_count) const {
	if (file_path.is_empty() || p_count == 0 || p_indices == nullptr) {
		return false;
	}

	// Find min/max indices to determine the contiguous read range.
	uint32_t min_idx = p_indices[0];
	uint32_t max_idx = p_indices[0];
	for (uint32_t i = 1; i < p_count; i++) {
		min_idx = MIN(min_idx, p_indices[i]);
		max_idx = MAX(max_idx, p_indices[i]);
	}
	if (max_idx >= splat_count) {
		ERR_PRINT(vformat("[StagedFileSource] Index out of bounds: max=%d total=%d",
				max_idx, splat_count));
		return false;
	}

	const uint32_t range_count = max_idx - min_idx + 1;
	const bool use_contiguous_range = uint64_t(range_count) <= uint64_t(p_count) * INDEXED_CONTIGUOUS_READ_MAX_OVERREAD_FACTOR;

	LocalVector<IndexedReadRequest> sparse_requests;
	if (!use_contiguous_range) {
		sparse_requests.resize(p_count);
		for (uint32_t i = 0; i < p_count; i++) {
			sparse_requests[i].index = p_indices[i];
			sparse_requests[i].output = i;
		}
		sparse_requests.sort_custom<IndexedReadRequestComparator>();
	}

	Ref<FileAccess> file = _get_thread_file();
	if (file.is_null()) {
		return false;
	}

	r_gaussians.resize(p_count);
	r_sh_first_order_count = sh_first_order;
	r_sh_high_order_count = sh_high_order;
	uint64_t logical_bytes_requested = uint64_t(p_count) * sizeof(Gaussian);
	uint64_t physical_bytes_read = 0;
	uint64_t got = 0;

	if (use_contiguous_range) {
		// Read the dense gaussian range covering all requested indices.
		const uint64_t gaussian_byte_offset = gaussian_data_offset + uint64_t(min_idx) * sizeof(Gaussian);
		const uint64_t gaussian_byte_count = uint64_t(range_count) * sizeof(Gaussian);

		LocalVector<Gaussian> range_buf;
		range_buf.resize(range_count);
		if (!_read_exact(file.ptr(), gaussian_byte_offset, range_buf.ptr(), gaussian_byte_count, "gaussians", &got)) {
			return false;
		}
		physical_bytes_read += got;

		for (uint32_t i = 0; i < p_count; i++) {
			r_gaussians[i] = range_buf[p_indices[i] - min_idx];
		}
	} else {
		LocalVector<Gaussian> run_buf;
		for (uint32_t i = 0; i < sparse_requests.size();) {
			const uint32_t run_start = sparse_requests[i].index;
			uint32_t run_end = run_start;
			uint32_t j = i + 1;
			while (j < sparse_requests.size() && _indexed_request_extends_run(sparse_requests[j].index, run_end)) {
				if (sparse_requests[j].index > run_end) {
					run_end = sparse_requests[j].index;
				}
				j++;
			}

			const uint32_t run_count = run_end - run_start + 1;
			const uint64_t gaussian_byte_offset = gaussian_data_offset + uint64_t(run_start) * sizeof(Gaussian);
			const uint64_t gaussian_byte_count = uint64_t(run_count) * sizeof(Gaussian);
			run_buf.resize(run_count);
			if (!_read_exact(file.ptr(), gaussian_byte_offset, run_buf.ptr(), gaussian_byte_count, "gaussians", &got)) {
				return false;
			}
			physical_bytes_read += got;

			for (uint32_t k = i; k < j; k++) {
				r_gaussians[sparse_requests[k].output] = run_buf[sparse_requests[k].index - run_start];
			}
			i = j;
		}
	}

	if (sh_high_order > 0 && sh_data_offset > 0) {
		const uint64_t sh_per_splat = uint64_t(sh_high_order);
		const uint64_t out_element_count = uint64_t(p_count) * sh_per_splat;
		uint32_t out_element_count_u32 = 0;
		if (!_sh_capture_count_fits_u32(out_element_count, "indexed-output", out_element_count_u32)) {
			return false;
		}
		const uint64_t logical_sh_byte_count = uint64_t(out_element_count_u32) * sizeof(Vector3);
		r_sh_high_order_out.resize(out_element_count_u32);
		logical_bytes_requested += logical_sh_byte_count;

		if (use_contiguous_range) {
			const uint64_t range_element_count = uint64_t(range_count) * sh_per_splat;
			uint32_t range_element_count_u32 = 0;
			if (!_sh_capture_count_fits_u32(range_element_count, "indexed-range", range_element_count_u32)) {
				return false;
			}
			const uint64_t sh_byte_offset = sh_data_offset + uint64_t(min_idx) * sh_per_splat * sizeof(Vector3);
			// Resize width and copy width both derive from the same u32-verified count.
			const uint64_t sh_byte_count = uint64_t(range_element_count_u32) * sizeof(Vector3);

			LocalVector<Vector3> sh_range_buf;
			sh_range_buf.resize(range_element_count_u32);
			if (!_read_exact(file.ptr(), sh_byte_offset, sh_range_buf.ptr(), sh_byte_count, "SH data", &got)) {
				return false;
			}
			physical_bytes_read += got;

			for (uint32_t i = 0; i < p_count; i++) {
				const uint32_t src_base = (p_indices[i] - min_idx) * uint32_t(sh_per_splat);
				const uint32_t dst_base = i * uint32_t(sh_per_splat);
				for (uint32_t c = 0; c < uint32_t(sh_per_splat); c++) {
					r_sh_high_order_out[dst_base + c] = sh_range_buf[src_base + c];
				}
			}
		} else {
			LocalVector<Vector3> sh_run_buf;
			for (uint32_t i = 0; i < sparse_requests.size();) {
				const uint32_t run_start = sparse_requests[i].index;
				uint32_t run_end = run_start;
				uint32_t j = i + 1;
				while (j < sparse_requests.size() && _indexed_request_extends_run(sparse_requests[j].index, run_end)) {
					if (sparse_requests[j].index > run_end) {
						run_end = sparse_requests[j].index;
					}
					j++;
				}

				const uint32_t run_count = run_end - run_start + 1;
				const uint64_t run_element_count = uint64_t(run_count) * sh_per_splat;
				uint32_t run_element_count_u32 = 0;
				if (!_sh_capture_count_fits_u32(run_element_count, "indexed-run", run_element_count_u32)) {
					return false;
				}
				const uint64_t sh_byte_offset = sh_data_offset + uint64_t(run_start) * sh_per_splat * sizeof(Vector3);
				// Resize width and copy width both derive from the same u32-verified count.
				const uint64_t sh_byte_count = uint64_t(run_element_count_u32) * sizeof(Vector3);
				sh_run_buf.resize(run_element_count_u32);
				if (!_read_exact(file.ptr(), sh_byte_offset, sh_run_buf.ptr(), sh_byte_count, "SH data", &got)) {
					return false;
				}
				physical_bytes_read += got;

				for (uint32_t k = i; k < j; k++) {
					const uint32_t src_base = (sparse_requests[k].index - run_start) * uint32_t(sh_per_splat);
					const uint32_t dst_base = sparse_requests[k].output * uint32_t(sh_per_splat);
					for (uint32_t c = 0; c < uint32_t(sh_per_splat); c++) {
						r_sh_high_order_out[dst_base + c] = sh_run_buf[src_base + c];
					}
				}
				i = j;
			}
		}
	} else {
		r_sh_high_order_out.clear();
	}

	_record_io_counters(logical_bytes_requested, physical_bytes_read);
	return true;
}
