#include "streaming_chunk_payload_source.h"

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
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

StagedFileChunkPayloadSource::ScopedReaderSuspend::ScopedReaderSuspend(const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}
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
		source->file_mutex.lock();
		const String source_path = settings ? settings->globalize_path(source->file_path) : source->file_path;
		if (source_path != target) {
			source->file_mutex.unlock();
			continue;
		}
		// Drop the cached handles so the OS closes them, and keep file_mutex held so
		// _get_thread_file() cannot reopen before the caller's rename completes.
		source->cached_files.clear();
		held.push_back(source);
	}
}

StagedFileChunkPayloadSource::ScopedReaderSuspend::~ScopedReaderSuspend() {
	for (uint32_t i = 0; i < held.size(); i++) {
		held[i]->file_mutex.unlock();
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
	const Thread::ID thread_id = Thread::get_caller_id();
	MutexLock lock(file_mutex);

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
