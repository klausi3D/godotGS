#include "gs_payload_read_service.h"

#include "core/error/error_macros.h"
#include "core/os/os.h"

#include <chrono>

namespace {
uint64_t read_clock_nsec() {
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

PayloadReadService::~PayloadReadService() {
	shutdown();
}

Error PayloadReadService::start(const Config &p_config) {
	ERR_FAIL_COND_V(state != SERVICE_NEW, ERR_ALREADY_IN_USE);
	ERR_FAIL_COND_V(p_config.reader_count == 0 || p_config.max_bytes_in_flight == 0, ERR_INVALID_PARAMETER);

	config = p_config;
	state = SERVICE_RUNNING;
	readers.reserve(config.reader_count);
	for (uint32_t i = 0; i < config.reader_count; i++) {
		Thread *reader = memnew(Thread);
		if (reader->start(_reader_entry, this) == Thread::UNASSIGNED_ID) {
			memdelete(reader);
			shutdown();
			return ERR_UNAVAILABLE;
		}
		readers.push_back(reader);
	}
	return OK;
}

void PayloadReadService::shutdown() {
	{
		MutexLock lock(mutex);
		if (state != SERVICE_RUNNING) {
			if (state == SERVICE_NEW) {
				state = SERVICE_STOPPED;
			}
			return;
		}
		state = SERVICE_STOPPING;
		stopping.set();
		work_cv.notify_all();
		ready_cv.notify_all();
		while (waiters > 0) {
			ready_cv.wait(lock);
		}
	}

	// Joined outside the mutex: a reader needs it to hand back its request.
	for (Thread *reader : readers) {
		reader->wait_to_finish();
		memdelete(reader);
	}
	readers.clear();

	// With the readers gone, every live request sits in one of the two lists.
	LocalVector<IdleHandle> closing;
	{
		MutexLock lock(mutex);
		// A cancellation drain may have entered while shutdown was joining
		// readers. Their acknowledgements now exist; let those waiters leave
		// before destroying service state as well as the original ready waiters.
		while (waiters > 0) {
			ready_cv.wait(lock);
		}
		for (SelfList<Request>::List *list : { &pending, &ready }) {
			while (SelfList<Request> *element = list->first()) {
				Request *request = element->self();
				list->remove(element);
				counters.discarded_on_shutdown++;
				memdelete(request);
			}
		}
		tickets.clear();
		closing = idle_handles;
		idle_handles.clear();
		counters.queued = 0;
		counters.ready = 0;
		counters.bytes_in_flight = 0;
		counters.open_handles = 0;
		state = SERVICE_STOPPED;
	}
}

PayloadReadService::SubmitResult PayloadReadService::submit(const String &p_file, uint64_t p_offset, uint64_t p_length, uint64_t p_ticket) {
	// FileAccess seeks through a signed position: past INT64_MAX a seek fails silently and
	// the read would return bytes from the old cursor.
	if (p_file.is_empty() || p_length == 0 || p_offset > uint64_t(INT64_MAX) || p_length > uint64_t(INT64_MAX) - p_offset) {
		return SUBMIT_INVALID;
	}

	MutexLock lock(mutex);
	if (state != SERVICE_RUNNING) {
		return SUBMIT_NOT_RUNNING;
	}
	if (p_length > config.max_bytes_in_flight) {
		return SUBMIT_TOO_LARGE;
	}
	if (tickets.has(p_ticket)) {
		return SUBMIT_DUPLICATE_TICKET;
	}
	if (p_length > config.max_bytes_in_flight - counters.bytes_in_flight) {
		counters.refused_busy++;
		return SUBMIT_BUSY;
	}

	Request *request = memnew(Request);
	request->file = p_file;
	request->offset = p_offset;
	request->length = p_length;
	request->ticket = p_ticket;
	request->queued_nsec = read_clock_nsec();
	tickets.insert(p_ticket, request);
	pending.add_last(&request->link);

	counters.submitted++;
	counters.queued++;
	counters.bytes_in_flight += p_length;
	counters.peak_bytes_in_flight = MAX(counters.peak_bytes_in_flight, counters.bytes_in_flight);
	work_cv.notify_one();
	return SUBMIT_OK;
}

bool PayloadReadService::cancel(uint64_t p_ticket) {
	MutexLock lock(mutex);
	HashMap<uint64_t, Request *>::Iterator found = tickets.find(p_ticket);
	if (!found) {
		return false;
	}
	Request *request = found->value;
	_cancel_locked(request);
	return true;
}

void PayloadReadService::cancel_and_wait(const LocalVector<uint64_t> &p_tickets) {
	MutexLock lock(mutex);
	LocalVector<Request *> draining;
	for (uint64_t ticket : p_tickets) {
		HashMap<uint64_t, Request *>::Iterator found = tickets.find(ticket);
		if (!found) {
			continue;
		}
		Request *request = found->value;
		if (request->state == REQUEST_READING) {
			request->cancel_waiting = true;
			draining.push_back(request);
		}
		_cancel_locked(request);
	}
	if (draining.is_empty()) {
		return;
	}

	// Every sibling has already been cancelled. Readers cannot acquire `mutex`
	// until this waiter is counted, and transfer deletion only after their ack.
	waiters++;
	for (Request *request : draining) {
		while (!request->read_drained) {
			ready_cv.wait(lock);
		}
		memdelete(request);
	}
	waiters--;
	ready_cv.notify_all(); // shutdown() may be waiting for the last drain waiter.
}

void PayloadReadService::_cancel_locked(Request *p_request) {
	tickets.erase(p_request->ticket);
	ready_cv.notify_all(); // A wait_ready() may be watching this exact ticket.

	switch (p_request->state) {
		case REQUEST_QUEUED:
			pending.remove(&p_request->link);
			counters.queued--;
			counters.cancelled_queued++;
			break;
		case REQUEST_READING:
			// The reader frees its budget when it stops. A drain waiter, when present,
			// owns request deletion; ordinary cancellation leaves deletion to the reader.
			p_request->cancelled.set();
			counters.cancelled_reading++;
			return;
		case REQUEST_READY:
			ready.remove(&p_request->link);
			counters.ready--;
			counters.cancelled_ready++;
			break;
	}
	counters.bytes_in_flight -= p_request->length;
	memdelete(p_request);
}

PayloadReadService::Stats PayloadReadService::stats() const {
	MutexLock lock(mutex);
	return counters;
}

bool PayloadReadService::poll_completion(Completion &r_completion) {
	MutexLock lock(mutex);
	if (ready.first() == nullptr) {
		return false;
	}
	_take_ready_locked(r_completion);
	return true;
}

bool PayloadReadService::wait_completion(Completion &r_completion) {
	MutexLock lock(mutex);
	waiters++;
	while (state == SERVICE_RUNNING && ready.first() == nullptr) {
		ready_cv.wait(lock);
	}
	const bool taken = state == SERVICE_RUNNING;
	if (taken) {
		_take_ready_locked(r_completion);
	}
	waiters--;
	if (!taken) {
		ready_cv.notify_all(); // shutdown() may be waiting for the last waiter.
	}
	return taken;
}

bool PayloadReadService::wait_ready(uint64_t p_watch_ticket) {
	MutexLock lock(mutex);
	waiters++;
	while (state == SERVICE_RUNNING && ready.first() == nullptr &&
			(p_watch_ticket == 0 || tickets.has(p_watch_ticket))) {
		ready_cv.wait(lock);
	}
	const bool available = state == SERVICE_RUNNING && ready.first() != nullptr &&
			(p_watch_ticket == 0 || tickets.has(p_watch_ticket));
	waiters--;
	if (!available) {
		ready_cv.notify_all(); // shutdown() may be waiting for the last waiter.
	}
	return available;
}

void PayloadReadService::release_handles() {
	// Declared before the lock, so the handles close after it unlocks.
	LocalVector<IdleHandle> closing;
	MutexLock lock(mutex);
	handle_epoch++;
	counters.open_handles -= idle_handles.size();
	closing = idle_handles;
	idle_handles.clear();
}

Ref<FileAccess> PayloadReadService::_take_idle_handle_locked(const String &p_file) {
	for (int64_t i = int64_t(idle_handles.size()) - 1; i >= 0; i--) {
		if (idle_handles[i].file == p_file) {
			Ref<FileAccess> handle = idle_handles[i].handle;
			idle_handles.remove_at(i);
			return handle;
		}
	}
	return Ref<FileAccess>();
}

void PayloadReadService::_take_ready_locked(Completion &r_completion) {
	Request *request = ready.first()->self();
	ready.remove(&request->link);
	tickets.erase(request->ticket);
	counters.ready--;
	counters.bytes_in_flight -= request->length;

	r_completion.ticket = request->ticket;
	r_completion.error = request->error;
	r_completion.bytes = request->bytes;
	memdelete(request);
	ready_cv.notify_all(); // A wait_ready() watching this ticket must not remain blocked.
}

void PayloadReadService::_reader_entry(void *p_self) {
	Thread::set_name("GS payload reader");
	static_cast<PayloadReadService *>(p_self)->_reader_loop();
}

Error PayloadReadService::_read(Request &p_request, Ref<FileAccess> &r_handle, Vector<uint8_t> &r_bytes, uint64_t &r_read, bool &r_opened) {
	if (r_handle.is_null()) {
		Error open_error = OK;
		r_handle = FileAccess::open(p_request.file, FileAccess::READ, &open_error);
		if (r_handle.is_null()) {
			return open_error == OK ? ERR_CANT_OPEN : open_error;
		}
		r_opened = true;
	}

	const Error alloc_error = r_bytes.resize_uninitialized(int64_t(p_request.length));
	if (alloc_error != OK) {
		return alloc_error;
	}

	r_handle->seek(p_request.offset);
	uint8_t *destination = r_bytes.ptrw();
	while (r_read < p_request.length) {
		if (p_request.cancelled.is_set() || stopping.is_set()) {
			return ERR_SKIP;
		}
#ifdef TESTS_ENABLED
		if (test_read_hook != nullptr) {
			test_read_hook(test_read_hook_user, p_request.ticket, r_read);
		}
#endif
		const uint64_t slice = MIN(read_slice_bytes, p_request.length - r_read);
		const uint64_t got = r_handle->get_buffer(destination + r_read, slice);
		r_read += got;
		if (got != slice) {
			return ERR_FILE_EOF;
		}
	}
	return OK;
}

void PayloadReadService::_reader_loop() {
	while (true) {
		Request *request = nullptr;
		Ref<FileAccess> handle;
		uint64_t leased_epoch = 0;
#ifdef TESTS_ENABLED
		uint32_t delay_usec = 0;
#endif
		{
			MutexLock lock(mutex);
			while (!stopping.is_set() && pending.first() == nullptr) {
				work_cv.wait(lock);
			}
			if (stopping.is_set()) {
				return;
			}
			request = pending.first()->self();
			pending.remove(&request->link);
			request->state = REQUEST_READING;
			counters.queue_nsec += read_clock_nsec() - request->queued_nsec;
#ifdef TESTS_ENABLED
			delay_usec = test_read_delay_usec;
#endif
			counters.queued--;
			counters.reading++;
			handle = _take_idle_handle_locked(request->file);
			leased_epoch = handle_epoch;
			if (handle.is_valid()) {
				counters.handle_reuses++;
			}
		}

		Vector<uint8_t> bytes;
		uint64_t bytes_read = 0;
		bool opened = false;
		const uint64_t read_started = read_clock_nsec();
#ifdef TESTS_ENABLED
		uint64_t delay_nsec = 0;
		if (delay_usec != 0) {
			OS::get_singleton()->delay_usec(delay_usec);
			delay_nsec = read_clock_nsec() - read_started;
		}
#endif
		const Error error = _read(*request, handle, bytes, bytes_read, opened);
		const uint64_t read_elapsed = read_clock_nsec() - read_started;
		// ERR_SKIP is a stop, not an I/O failure: the handle is healthy.
		const bool stopped = error == ERR_SKIP;

		// Declared before the lock, so a close never runs under the mutex.
		IdleHandle evicted;
		MutexLock lock(mutex);
		counters.reading--;
		counters.read_nsec += read_elapsed;
#ifdef TESTS_ENABLED
		if (delay_usec != 0) {
			++counters.test_read_delay_calls;
			counters.test_read_delay_nsec += delay_nsec;
		}
#endif
		counters.bytes_read += bytes_read;
		if (opened) {
			counters.handle_opens++;
			counters.open_handles++;
		}

		if (handle.is_valid()) {
			if ((error == OK || stopped) && leased_epoch == handle_epoch && config.max_idle_handles > 0) {
				if (idle_handles.size() >= config.max_idle_handles) {
					evicted = idle_handles[0];
					idle_handles.remove_at(0);
					counters.open_handles--;
				}
				IdleHandle idle;
				idle.file = request->file;
				idle.handle = handle;
				idle_handles.push_back(idle);
				// The pool owns it now. release_handles() must be able to close it the moment
				// this completion is taken, so no reference may outlive the lock.
				handle.unref();
			} else {
				counters.open_handles--;
				// `handle` outlives the lock (it is declared before it) and closes after.
			}
		}

		if (request->cancelled.is_set()) {
			bytes.clear(); // The budget comes back only once the buffer is really gone.
			counters.bytes_in_flight -= request->length;
			if (request->cancel_waiting) {
				// Only the explicit drain API waits for handle relinquishment. Keep
				// ordinary completion publication and nonblocking cancel unchanged.
				lock.temp_unlock();
				handle.unref();
				evicted.handle.unref();
				lock.temp_relock();
				request->read_drained = true;
				ready_cv.notify_all();
			} else {
				memdelete(request);
			}
			continue;
		}

		request->error = error;
		if (error == OK) {
			request->bytes = bytes;
			counters.completed_ok++;
		} else if (!stopped) {
			counters.completed_failed++;
		}
		// READY must have no untracked reader-local storage owner. On success
		// the request holds the bytes; on failure no output buffer survives.
		bytes.clear();
		request->state = REQUEST_READY;
		ready.add_last(&request->link);
		counters.ready++;
		// Drain waiters also use this condition variable and may not consume work.
		ready_cv.notify_all();
	}
}

#ifdef TESTS_ENABLED
Error PayloadReadService::_test_set_read_delay(uint32_t p_usec) {
	MutexLock lock(mutex);
	if (counters.queued || counters.reading || counters.ready) { return ERR_BUSY; }
	test_read_delay_usec = p_usec;
	return OK;
}
#endif
