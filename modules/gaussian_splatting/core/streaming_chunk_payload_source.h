#ifndef STREAMING_CHUNK_PAYLOAD_SOURCE_H
#define STREAMING_CHUNK_PAYLOAD_SOURCE_H

#include "core/io/file_access.h"
#include "core/math/aabb.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "gaussian_data.h"
#include <cstdint>

// Abstract source of chunk gaussian payloads for the streaming system.
// Implementations provide either in-memory reads (wrapping GaussianData)
// or on-demand file-backed reads for out-of-core large-world streaming.
class ChunkPayloadSource : public RefCounted {
	GDCLASS(ChunkPayloadSource, RefCounted);

protected:
	static void _bind_methods();

public:
	virtual ~ChunkPayloadSource() = default;

	virtual bool capture_chunk_snapshot(uint32_t p_start, uint32_t p_count,
			LocalVector<Gaussian> &r_gaussians,
			LocalVector<Vector3> &r_sh_high_order,
			uint32_t &r_sh_first_order_count,
			uint32_t &r_sh_high_order_count) const = 0;

	virtual bool capture_indexed_chunk_snapshot(const uint32_t *p_indices, uint32_t p_count,
			LocalVector<Gaussian> &r_gaussians,
			LocalVector<Vector3> &r_sh_high_order,
			uint32_t &r_sh_first_order_count,
			uint32_t &r_sh_high_order_count) const = 0;

	virtual uint32_t get_count() const = 0;
	virtual uint32_t get_sh_degree() const = 0;
	virtual AABB get_bounds() const = 0;
	virtual bool is_valid() const = 0;
};

// Wraps an in-memory GaussianData. This is the default path for small
// datasets and smoke tests — identical behaviour to the pre-existing code.
class InMemoryChunkPayloadSource : public ChunkPayloadSource {
	GDCLASS(InMemoryChunkPayloadSource, ChunkPayloadSource);
	Ref<GaussianData> data;

protected:
	static void _bind_methods();

public:
	InMemoryChunkPayloadSource() = default;
	explicit InMemoryChunkPayloadSource(const Ref<GaussianData> &p_data) :
			data(p_data) {}

	void set_data(const Ref<GaussianData> &p_data) { data = p_data; }
	Ref<GaussianData> get_data() const { return data; }

	bool capture_chunk_snapshot(uint32_t p_start, uint32_t p_count,
			LocalVector<Gaussian> &r_gaussians,
			LocalVector<Vector3> &r_sh_high_order,
			uint32_t &r_sh_first_order_count,
			uint32_t &r_sh_high_order_count) const override;

	bool capture_indexed_chunk_snapshot(const uint32_t *p_indices, uint32_t p_count,
			LocalVector<Gaussian> &r_gaussians,
			LocalVector<Vector3> &r_sh_high_order,
			uint32_t &r_sh_first_order_count,
			uint32_t &r_sh_high_order_count) const override;

	uint32_t get_count() const override;
	uint32_t get_sh_degree() const override;
	AABB get_bounds() const override;
	bool is_valid() const override;
};

// Reads chunk gaussian payloads directly from an uncompressed
// .gsplatworld file on demand.  Only the file path, header offsets,
// and lightweight metadata are kept in memory — the full gaussian
// array is never loaded, so process memory does not scale with
// total world splat count.
class StagedFileChunkPayloadSource : public ChunkPayloadSource {
	GDCLASS(StagedFileChunkPayloadSource, ChunkPayloadSource);
	String file_path;

protected:
	static void _bind_methods();
	uint64_t gaussian_data_offset = 0; // byte offset of gaussian array in file
	uint64_t sh_data_offset = 0; // byte offset of SH array (0 = no SH)
	uint32_t splat_count = 0;
	uint32_t sh_degree = 0;
	uint32_t sh_first_order = 0;
	uint32_t sh_high_order = 0;
	AABB bounds;
	mutable Mutex file_mutex;
	mutable HashMap<Thread::ID, Ref<FileAccess>> cached_files;
	// #714: set while a writer is atomically replacing file_path. Lock-free on
	// purpose: _get_thread_file() must be able to consult it WITHOUT holding
	// file_mutex, and ScopedReaderSuspend must be able to set it without holding
	// file_mutex either -- an already-active read still needs that mutex to finish
	// (_record_io_counters takes it while the reader's Ref is alive), so blocking on
	// it would stop the very reads the guard is waiting to drain.
	mutable SafeFlag suspend_opens;
	mutable uint64_t bytes_requested = 0;
	mutable uint64_t bytes_read = 0;
	mutable uint64_t file_open_count = 0;

	// #714: registry of live file-backed sources, so a writer replacing an imported
	// .gsplatworld can find the readers holding it open. Guarded by its own static
	// mutex (see _registry_mutex()) because it is process-wide state, not per-source
	// state, so it cannot use file_mutex.
	//
	// LOCK ORDER: registry mutex -> instance file_mutex, never the reverse. No code
	// holding file_mutex may touch the registry.
	static Mutex &_registry_mutex();
	static HashMap<StagedFileChunkPayloadSource *, bool> &_live_sources();

	Ref<FileAccess> _get_thread_file() const;
	bool _read_exact(FileAccess *p_file, uint64_t p_offset, void *p_dst, uint64_t p_bytes, const char *p_label, uint64_t *r_bytes_read = nullptr) const;
	void _record_io_counters(uint64_t p_bytes_requested, uint64_t p_bytes_read) const;

public:
	StagedFileChunkPayloadSource();
	~StagedFileChunkPayloadSource() override;

	// #714: quiesces every live file-backed source reading `p_path` for this object's
	// lifetime, so a writer can atomically replace that file.
	//
	// Windows opens FileAccess without FILE_SHARE_DELETE (file_access_windows.cpp:
	// _SH_DENYNO / _SH_DENYWR / _SH_DENYRW), so an open reader blocks MoveFileExW
	// replacement AND the backup-swap fallback rename. Merely dropping the cached
	// handles is not enough: _get_thread_file() would immediately reopen and re-cache
	// mid-copy, making reimport intermittent under continuous streaming.
	//
	// So this sets each matching source's lock-free `suspend_opens` flag (which stops
	// NEW opens), takes over the cached handles, and then WAITS for reads already in
	// flight to release their own Refs before returning to the caller's rename.
	//
	// It deliberately does NOT hold file_mutex while waiting. A read already past
	// _get_thread_file() still needs that mutex to finish -- _record_io_counters()
	// takes it while the reader's Ref is alive -- so holding it would block the very
	// reads being drained and make the timeout certain instead of rare.
	//
	// LOCK ORDER: registry mutex -> instance file_mutex, and file_mutex is only ever
	// taken briefly, never across the wait. The registry mutex IS held for the
	// guard's lifetime, purely so a source cannot be destroyed mid-drain.
	//
	// Scope it to the RENAME ONLY, never across the copy: the wait is bounded, but
	// the copy is file-sized.
	// The wait is bounded, so it can expire with a reader still holding the file open
	// (a read slower than the drain budget, or one that never returns). That outcome
	// is reported by drained() and is part of the contract rather than a caveat:
	// gs_atomic_file_write() refuses the replace instead of attempting a rename whose
	// precondition is known to be violated, so the timeout tunes how long an import
	// may stall, NOT whether the result is correct.
	//
	// WINDOWS ONLY in effect. The constraint being worked around is Windows-specific
	// (FileAccessWindows opens via _wfsopen with _SH_DENY*, none of which share delete),
	// whereas POSIX rename() replaces the destination with readers still open -- they
	// keep the old inode until they close. On POSIX the constructor is therefore an
	// early-out no-op that reports drained; suspending and draining there would burn up
	// to the drain budget per reimport and could refuse a replace that would have
	// succeeded.
	class ScopedReaderSuspend {
		LocalVector<StagedFileChunkPayloadSource *> held;
		// False when constructed with an empty path, which locks nothing at all.
		bool registry_locked = false;
		// True unless the bounded wait expired with a handle still in flight. Starts
		// true so the "nothing to drain" case (no matching source) is drained.
		bool readers_drained = true;

	public:
		explicit ScopedReaderSuspend(const String &p_path);
		~ScopedReaderSuspend();

		ScopedReaderSuspend(const ScopedReaderSuspend &) = delete;
		ScopedReaderSuspend &operator=(const ScopedReaderSuspend &) = delete;

		uint32_t suspended_count() const { return held.size(); }
		// False => at least one reader still had the destination open when the wait
		// expired. The caller must NOT attempt the replace; see gs_atomic_file_writer.h.
		bool drained() const { return readers_drained; }
	};

	void configure(const String &p_path,
			uint64_t p_gaussian_offset,
			uint64_t p_sh_offset,
			uint32_t p_splat_count,
			uint32_t p_sh_degree,
			uint32_t p_sh_first_order,
			uint32_t p_sh_high_order,
			const AABB &p_bounds);

	bool capture_chunk_snapshot(uint32_t p_start, uint32_t p_count,
			LocalVector<Gaussian> &r_gaussians,
			LocalVector<Vector3> &r_sh_high_order,
			uint32_t &r_sh_first_order_count,
			uint32_t &r_sh_high_order_count) const override;

	bool capture_indexed_chunk_snapshot(const uint32_t *p_indices, uint32_t p_count,
			LocalVector<Gaussian> &r_gaussians,
			LocalVector<Vector3> &r_sh_high_order,
			uint32_t &r_sh_first_order_count,
			uint32_t &r_sh_high_order_count) const override;

	uint32_t get_count() const override { return splat_count; }
	uint32_t get_sh_degree() const override { return sh_degree; }
	AABB get_bounds() const override { return bounds; }
	bool is_valid() const override { return !file_path.is_empty() && splat_count > 0; }

	const String &get_file_path() const { return file_path; }
	uint64_t get_bytes_requested() const;
	uint64_t get_bytes_read() const;
	uint64_t get_file_open_count() const;
	void reset_io_counters();
};

#endif // STREAMING_CHUNK_PAYLOAD_SOURCE_H
