#pragma once

#include "core/error/error_list.h"
#include "core/io/file_access.h"
#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/self_list.h"
#include "core/templates/vector.h"

#include <cstdint>

// Positional payload reads on a bounded reader pool
// (docs/architecture/adr-beyond-vram-worlds.md §3.1, §3.2, §3.4 step 5; BV-08b).
//
// BV-08b shipped the service alone. Its first runtime caller is the paged-GSQ staged source
// (BV-C5b, core/streaming_gsq_chunk_payload_source.h).
//
// Contract:
//
// - submit() never blocks and never waits for budget. Every byte of a request is charged
//   to max_bytes_in_flight from an accepted submit() until its completion is taken or it
//   is cancelled, so a consumer that stops taking completions stops further admission
//   and nothing piles up behind the readers. A caller that gets SUBMIT_BUSY skips the
//   request and asks again next frame, the same rule as run admission (I-2).
// - A read is positional: a reader leases a handle exclusively, seeks and reads, so
//   concurrent reads of one file never share a cursor.
// - Handles are pooled per path across readers. At most reader_count + max_idle_handles
//   are open at once; the least recently used idle handle closes first.
// - cancel() is exact: once it returns true, that ticket is never delivered. A read
//   already running stops at its next slice boundary (read_slice_bytes) and keeps its
//   budget until then, because its buffer is still allocated. Its ticket can be submitted
//   again at once.
// - A completion carries either every requested byte (error == OK) or none. A short read
//   is ERR_FILE_EOF, and the handle that saw a failure is closed rather than pooled.
// - Tickets are opaque to the service and unique among live requests. Generation checks
//   (I-5) stay with the residency owner, which encodes the generation in the ticket.
//
// Threading: submit(), cancel(), cancel_and_wait(), stats(), poll_completion(),
// wait_completion(), wait_ready() and release_handles() are safe from any thread.
// start() and shutdown() belong to the owner,
// who must not call anything else concurrently with destruction: shutdown() wakes and
// waits out completion, ready and cancellation-drain waiters, but cannot protect a call that
// starts after the object is gone. `mutex` guards every field declared after it; file
// I/O runs outside it.
class PayloadReadService {
public:
	// A reader observes cancel and shutdown between slices.
	static constexpr uint64_t read_slice_bytes = 1024 * 1024;

	struct Config {
		uint32_t reader_count = 4;
		uint64_t max_bytes_in_flight = 64 * 1024 * 1024;
		uint32_t max_idle_handles = 16;
	};

	enum SubmitResult {
		SUBMIT_OK,
		SUBMIT_BUSY, // Budget is full right now; retrying later can succeed.
		SUBMIT_TOO_LARGE, // Longer than max_bytes_in_flight; retrying can never succeed.
		SUBMIT_INVALID, // Empty path, zero length, or offset + length past INT64_MAX.
		SUBMIT_DUPLICATE_TICKET, // That ticket is still live.
		SUBMIT_NOT_RUNNING, // Not started, or shut down.
	};

	struct Completion {
		uint64_t ticket = 0;
		Error error = OK;
		Vector<uint8_t> bytes;
	};

	struct Stats {
		uint64_t submitted = 0;
		uint64_t refused_busy = 0;
		uint64_t completed_ok = 0;
		uint64_t completed_failed = 0;
		uint64_t cancelled_queued = 0;
		uint64_t cancelled_reading = 0;
		uint64_t cancelled_ready = 0;
		uint64_t discarded_on_shutdown = 0;
		uint64_t bytes_read = 0;
		// CPU wall latency: enqueue -> reader admission, then actual IO only.
		// Protected by mutex, like the existing completion counters.
		uint64_t queue_nsec = 0;
		uint64_t read_nsec = 0;
#ifdef TESTS_ENABLED
		uint64_t test_read_delay_calls = 0;
		uint64_t test_read_delay_nsec = 0;
#endif
		uint64_t handle_opens = 0;
		uint64_t handle_reuses = 0;
		uint64_t open_handles = 0;
		uint64_t bytes_in_flight = 0;
		uint64_t peak_bytes_in_flight = 0;
		uint32_t queued = 0;
		uint32_t reading = 0;
		uint32_t ready = 0;
		uint32_t peak_jobs = 0; // Under mutex, current live requests include ready completions.
		uint64_t ready_bytes = 0, peak_ready_bytes = 0; // Successful completions retained by this service.
		uint64_t collection_peak_bytes = 0, collection_peak_ready_bytes = 0;
		uint32_t collection_peak_jobs = 0;
	};

	PayloadReadService() = default;
	~PayloadReadService();

	PayloadReadService(const PayloadReadService &) = delete;
	PayloadReadService &operator=(const PayloadReadService &) = delete;

	// All or nothing. One service starts once. ERR_UNAVAILABLE when a reader thread cannot start.
	Error start(const Config &p_config);
	// Stops the readers (a running read stops at its next slice), discards every request
	// and result, closes every handle and joins. Idempotent.
	void shutdown();

	SubmitResult submit(const String &p_file, uint64_t p_offset, uint64_t p_length, uint64_t p_ticket);
	// True when the ticket was live. False when it is unknown or already taken.
	bool cancel(uint64_t p_ticket);
	// Cancels every live listed ticket before waiting for any running read to drain.
	// Running readers acknowledge after freeing their buffer and relinquishing
	// their handle. Ordinary READY publication/handle-close timing stays unchanged.
	// Missing or repeated tickets are harmless, as for repeated single cancellations.
	void cancel_and_wait(const LocalVector<uint64_t> &p_tickets);
	Stats stats() const;
	void reset_observation_peaks(); // Owner collection boundary; does not alter admission or requests.

	// Takes the oldest result without blocking. Taking it frees its budget.
	bool poll_completion(Completion &r_completion);
	// Blocks until a result exists. False when the service stops first.
	bool wait_completion(Completion &r_completion);
	// Waits without taking a completion or releasing its byte reservation. A nonzero
	// watched ticket also wakes and returns false when it is cancelled or taken.
	// Zero watches only the service: ready work or shutdown ends the wait.
	bool wait_ready(uint64_t p_watch_ticket = 0);

	// Closes idle handles, and running reads drop theirs instead of pooling them. For a
	// writer about to replace a file (#714): an open handle denies the rename on Windows.
	// Cancel requests on that file first, or a queued read opens it again.
	void release_handles();

#ifdef TESTS_ENABLED
	Error _test_set_read_delay(uint32_t p_usec);
	// Runs on a reader thread, outside the mutex, before each slice. Set before start().
	typedef void (*ReadHook)(void *p_user, uint64_t p_ticket, uint64_t p_slice_offset);
	void _test_set_read_hook(ReadHook p_hook, void *p_user) {
		test_read_hook = p_hook;
		test_read_hook_user = p_user;
	}
#endif

private:
	enum RequestState {
		REQUEST_QUEUED,
		REQUEST_READING,
		REQUEST_READY,
	};

	struct Request {
		SelfList<Request> link;
		String file;
		uint64_t offset = 0;
		uint64_t length = 0;
		uint64_t ticket = 0;
		RequestState state = REQUEST_QUEUED;
		uint64_t queued_nsec = 0;
		Error error = OK;
		Vector<uint8_t> bytes;
		// Set under the mutex, read by the reader between slices without it. A cancelled
		// READING request has left `tickets`. Normally its reader deletes it; when
		// cancel_waiting is set, the cancelling waiter owns deletion after read_drained.
		SafeFlag cancelled;
		// Both guarded by `mutex`; the reader signals only after freeing its buffer
		// and releasing the charged bytes. The waiter then deletes this request.
		bool cancel_waiting = false;
		bool read_drained = false;

		Request() :
				link(this) {}
	};

	struct IdleHandle {
		String file;
		Ref<FileAccess> handle;
	};

	enum ServiceState {
		SERVICE_NEW,
		SERVICE_RUNNING,
		SERVICE_STOPPING,
		SERVICE_STOPPED,
	};

	static void _reader_entry(void *p_self);
	void _reader_loop();
	Error _read(Request &p_request, Ref<FileAccess> &r_handle, Vector<uint8_t> &r_bytes, uint64_t &r_read, bool &r_opened);
	Ref<FileAccess> _take_idle_handle_locked(const String &p_file);
	void _take_ready_locked(Completion &r_completion);
	void _cancel_locked(Request *p_request);

	Config config;
	LocalVector<Thread *> readers;
	SafeFlag stopping; // Set under the mutex, read by readers between slices without it.

#ifdef TESTS_ENABLED
	ReadHook test_read_hook = nullptr;
	void *test_read_hook_user = nullptr;
#endif

	mutable BinaryMutex mutex;
	ConditionVariable work_cv; // Readers: `pending` gained a request, or stopping.
	ConditionVariable ready_cv; // Completion/ready/drain waiters and shutdown share state-change notifications.
	ServiceState state = SERVICE_NEW;
	uint32_t waiters = 0; // Threads inside wait_completion(), wait_ready() or a cancellation drain.
	uint64_t handle_epoch = 0; // release_handles() bumps it; a read leased in an older epoch drops its handle.
	SelfList<Request>::List pending;
	SelfList<Request>::List ready;
	HashMap<uint64_t, Request *> tickets; // Every live ticket: QUEUED, READING or READY, never cancelled.
	LocalVector<IdleHandle> idle_handles; // Oldest first, at most config.max_idle_handles.
	Stats counters;
#ifdef TESTS_ENABLED
	uint32_t test_read_delay_usec = 0; // mutex; copied at dequeue, slept outside it.
#endif
};
