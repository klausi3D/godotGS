#pragma once

#include "test_macros.h"

#include "../io/gs_payload_read_service.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"

namespace {

using PRS = PayloadReadService;

constexpr uint64_t _PRS_DEADLINE_USEC = 20 * 1000 * 1000;

uint8_t _prs_byte_at(uint64_t p_position) {
	return uint8_t(p_position * 31u + (p_position >> 8) * 7u + (p_position >> 16) * 13u);
}

struct PrsFile {
	String path;

	PrsFile(const String &p_suffix, uint64_t p_size) {
		path = OS::get_singleton()->get_temp_path().path_join("gs_prs_" + p_suffix + "_" + itos(OS::get_singleton()->get_ticks_usec()) + ".bin");
		Vector<uint8_t> bytes;
		bytes.resize(p_size);
		uint8_t *write = bytes.ptrw();
		for (uint64_t i = 0; i < p_size; i++) {
			write[i] = _prs_byte_at(i);
		}
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_buffer(bytes.ptr(), p_size);
		}
	}
	~PrsFile() {
		DirAccess::remove_absolute(path);
	}
};

template <typename Condition>
bool _prs_wait_until(Condition p_condition) {
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + _PRS_DEADLINE_USEC;
	while (!p_condition()) {
		if (OS::get_singleton()->get_ticks_usec() > deadline) {
			return false;
		}
		OS::get_singleton()->delay_usec(100);
	}
	return true;
}

// Holds one ticket's read at its first slice boundary until released, so a case can act
// while a read is deterministically running. The hold has its own deadline: a failed case
// must not hang the destructor's join.
struct PrsReadGate {
	uint64_t ticket = 0;
	SafeFlag hold;
	SafeFlag entered;
	SafeNumeric<uint32_t> ticket_slices;

	static void hook(void *p_user, uint64_t p_ticket, uint64_t p_slice_offset) {
		PrsReadGate *gate = static_cast<PrsReadGate *>(p_user);
		if (p_ticket != gate->ticket) {
			return;
		}
		gate->ticket_slices.increment();
		gate->entered.set();
		_prs_wait_until([gate]() { return !gate->hold.is_set(); });
	}
};

bool _prs_take(PRS &p_service, PRS::Completion &r_completion) {
	return _prs_wait_until([&]() { return p_service.poll_completion(r_completion); });
}

bool _prs_bytes_match(const PRS::Completion &p_completion, uint64_t p_offset, uint64_t p_length) {
	if (p_completion.error != OK || uint64_t(p_completion.bytes.size()) != p_length) {
		return false;
	}
	const uint8_t *read = p_completion.bytes.ptr();
	for (uint64_t i = 0; i < p_length; i++) {
		if (read[i] != _prs_byte_at(p_offset + i)) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("[GaussianSplatting][PayloadReadService] concurrent reads of one file get the right bytes on pooled handles") {
	PrsFile file("positional", 256 * 1024);

	{
		// One reader, one file, serial reads: the second read onward must not open anything.
		PRS service;
		PRS::Config config;
		config.reader_count = 1;
		REQUIRE(service.start(config) == OK);
		for (uint64_t i = 0; i < 8; i++) {
			CHECK(service.submit(file.path, i * 4096, 4096, i) == PRS::SUBMIT_OK);
			PRS::Completion completion;
			if (!service.wait_completion(completion)) {
				FAIL("wait_completion returned false on a running service");
				return;
			}
			CHECK(completion.ticket == i);
			CHECK(_prs_bytes_match(completion, i * 4096, 4096));
		}
		const PRS::Stats stats = service.stats();
		CHECK(stats.handle_opens == 1);
		CHECK(stats.handle_reuses == 7);
		CHECK(stats.open_handles == 1);
		CHECK(stats.bytes_read == 8 * 4096);
	}

	{
		// Readers race on one path. A shared cursor would hand a reader another's bytes.
		PRS service;
		PRS::Config config;
		config.reader_count = 4;
		REQUIRE(service.start(config) == OK);
		const uint32_t count = 256;
		for (uint32_t i = 0; i < count; i++) {
			CHECK(service.submit(file.path, (i * 97u) % 4096u * 32u, 65536, i) == PRS::SUBMIT_OK);
		}
		uint32_t matched = 0;
		for (uint32_t i = 0; i < count; i++) {
			PRS::Completion completion;
			if (!_prs_take(service, completion)) {
				FAIL("a read never completed");
				return;
			}
			const uint32_t ticket = uint32_t(completion.ticket);
			matched += _prs_bytes_match(completion, (ticket * 97u) % 4096u * 32u, 65536) ? 1 : 0;
		}
		CHECK(matched == count);
		const PRS::Stats stats = service.stats();
		CHECK(stats.handle_opens <= config.reader_count);
		CHECK(stats.handle_opens + stats.handle_reuses == count);
	}
}

TEST_CASE("[GaussianSplatting][PayloadReadService] the idle pool is bounded and release_handles closes what it holds") {
	PrsFile a("idle_a", 4096);
	PrsFile b("idle_b", 4096);
	PRS service;
	PRS::Config config;
	config.reader_count = 1;
	config.max_idle_handles = 1;
	REQUIRE(service.start(config) == OK);

	PRS::Completion completion;
	const String order[3] = { a.path, b.path, a.path };
	for (uint32_t i = 0; i < 3; i++) {
		CHECK(service.submit(order[i], 0, 16, i) == PRS::SUBMIT_OK);
		if (!_prs_take(service, completion)) {
			FAIL("a read never completed");
			return;
		}
	}
	PRS::Stats stats = service.stats();
	CHECK(stats.handle_opens == 3); // b evicted a, and a evicted b.
	CHECK(stats.handle_reuses == 0);
	CHECK(stats.open_handles == 1);

	service.release_handles();
	CHECK(service.stats().open_handles == 0);
	// On Windows an open handle would refuse this delete.
	CHECK(DirAccess::remove_absolute(a.path) == OK);
}

TEST_CASE("[GaussianSplatting][PayloadReadService] bytes in flight stay bounded until the consumer takes them") {
	PrsFile file("budget", 4096);
	PrsReadGate gate;
	gate.ticket = 1;
	gate.hold.set();

	PRS service;
	service._test_set_read_hook(PrsReadGate::hook, &gate);
	PRS::Config config;
	config.reader_count = 1;
	config.max_bytes_in_flight = 1000;
	REQUIRE(service.start(config) == OK);

	CHECK(service.submit(file.path, 0, 600, 1) == PRS::SUBMIT_OK);
	CHECK(_prs_wait_until([&]() { return gate.entered.is_set(); }));
	CHECK(service.submit(file.path, 600, 400, 2) == PRS::SUBMIT_OK);
	CHECK(service.submit(file.path, 0, 1, 3) == PRS::SUBMIT_BUSY);
	CHECK(service.submit(file.path, 0, 1001, 3) == PRS::SUBMIT_TOO_LARGE);
	CHECK(service.submit(file.path, 0, 1, 2) == PRS::SUBMIT_DUPLICATE_TICKET);
	CHECK(service.stats().refused_busy == 1);

	// Finished reads still hold memory: the budget must not free until they are taken.
	gate.hold.clear();
	CHECK(_prs_wait_until([&]() { return service.stats().ready == 2; }));
	CHECK(service.stats().bytes_in_flight == 1000);
	CHECK(service.submit(file.path, 0, 1, 3) == PRS::SUBMIT_BUSY);

	PRS::Completion completion;
	CHECK(service.poll_completion(completion));
	CHECK(completion.ticket == 1);
	CHECK(_prs_bytes_match(completion, 0, 600));
	CHECK(service.stats().bytes_in_flight == 400);
	CHECK(service.submit(file.path, 0, 600, 3) == PRS::SUBMIT_OK);
	CHECK(service.submit(file.path, 0, 1, 4) == PRS::SUBMIT_BUSY);

	CHECK(_prs_take(service, completion));
	CHECK(_prs_take(service, completion));
	const PRS::Stats stats = service.stats();
	CHECK(stats.bytes_in_flight == 0);
	CHECK(stats.peak_bytes_in_flight == 1000);
	CHECK(stats.completed_ok == 3);
}

TEST_CASE("[GaussianSplatting][PayloadReadService] a cancelled ticket is never delivered, wherever it was") {
	const uint64_t slice = PRS::read_slice_bytes;
	PrsFile file("cancel", 3 * slice + 4096);
	PrsReadGate gate;
	gate.ticket = 1;
	gate.hold.set();

	PRS service;
	service._test_set_read_hook(PrsReadGate::hook, &gate);
	PRS::Config config;
	config.reader_count = 1;
	config.max_bytes_in_flight = 8 * slice;
	REQUIRE(service.start(config) == OK);

	CHECK(service.submit(file.path, 0, 3 * slice, 1) == PRS::SUBMIT_OK);
	CHECK(_prs_wait_until([&]() { return gate.entered.is_set(); }));
	CHECK(service.submit(file.path, 0, 1000, 2) == PRS::SUBMIT_OK);
	CHECK(service.submit(file.path, 4096, 1000, 3) == PRS::SUBMIT_OK);

	// Queued: gone at once, budget back at once.
	CHECK(service.cancel(2));
	CHECK_FALSE(service.cancel(2));
	CHECK_FALSE(service.cancel(777));
	CHECK(service.stats().bytes_in_flight == 3 * slice + 1000);

	// Running: its buffer stays charged until the reader stops, but the ticket is free.
	CHECK(service.cancel(1));
	CHECK(service.stats().bytes_in_flight == 3 * slice + 1000);
	CHECK(service.submit(file.path, 8192, 100, 1) == PRS::SUBMIT_OK);
	gate.hold.clear();

	PRS::Completion completion;
	CHECK(_prs_take(service, completion));
	CHECK(completion.ticket == 3);
	CHECK(_prs_bytes_match(completion, 4096, 1000));
	CHECK(_prs_take(service, completion));
	CHECK(completion.ticket == 1);
	CHECK(_prs_bytes_match(completion, 8192, 100)); // Not the cancelled 3-slice read.

	PRS::Stats stats = service.stats();
	// One slice of the cancelled read, then the resubmitted ticket 1: two hook calls, not
	// four, because the running read stopped at its next slice boundary.
	CHECK(gate.ticket_slices.get() == 2);
	CHECK(stats.bytes_read == slice + 1000 + 100);
	CHECK(stats.cancelled_queued == 1);
	CHECK(stats.cancelled_reading == 1);
	CHECK(stats.bytes_in_flight == 0);

	// Ready: withdrawn before it is taken.
	CHECK(service.submit(file.path, 0, 64, 4) == PRS::SUBMIT_OK);
	CHECK(_prs_wait_until([&]() { return service.stats().ready == 1; }));
	CHECK(service.cancel(4));
	CHECK_FALSE(service.poll_completion(completion));
	stats = service.stats();
	CHECK(stats.cancelled_ready == 1);
	CHECK(stats.bytes_in_flight == 0);
	CHECK(stats.completed_ok == 3); // Ticket 4 did finish; the cancel withdrew its result.
}

TEST_CASE("[GaussianSplatting][PayloadReadService] a failed read delivers no bytes and retires its handle") {
	PrsFile file("failure", 4096);
	PRS service;
	PRS::Config config;
	config.reader_count = 1;
	REQUIRE(service.start(config) == OK);

	CHECK(service.submit(String(), 0, 1, 1) == PRS::SUBMIT_INVALID);
	CHECK(service.submit(file.path, 0, 0, 1) == PRS::SUBMIT_INVALID);
	CHECK(service.submit(file.path, UINT64_MAX, 2, 1) == PRS::SUBMIT_INVALID);

	PRS::Completion completion;
	CHECK(service.submit(file.path + ".missing", 0, 16, 1) == PRS::SUBMIT_OK);
	CHECK(_prs_take(service, completion));
	CHECK(completion.error != OK);
	CHECK(completion.bytes.is_empty());

	CHECK(service.submit(file.path, 4000, 200, 2) == PRS::SUBMIT_OK); // Runs past the end.
	CHECK(_prs_take(service, completion));
	CHECK(completion.error == ERR_FILE_EOF);
	CHECK(completion.bytes.is_empty());
	CHECK(service.stats().open_handles == 0);

	CHECK(service.submit(file.path, 0, 16, 3) == PRS::SUBMIT_OK);
	CHECK(_prs_take(service, completion));
	CHECK(_prs_bytes_match(completion, 0, 16));
	const PRS::Stats stats = service.stats();
	CHECK(stats.completed_failed == 2);
	CHECK(stats.completed_ok == 1);
	CHECK(stats.handle_opens == 2);
	CHECK(stats.handle_reuses == 0);
}

namespace {

struct PrsWaiter {
	PRS *service = nullptr;
	SafeFlag returned;
	SafeFlag got_completion;

	static void run(void *p_user) {
		PrsWaiter *waiter = static_cast<PrsWaiter *>(p_user);
		PRS::Completion completion;
		if (waiter->service->wait_completion(completion)) {
			waiter->got_completion.set();
		}
		waiter->returned.set();
	}
};

struct PrsStopper {
	PRS *service = nullptr;
	SafeFlag returned;

	static void run(void *p_user) {
		PrsStopper *stopper = static_cast<PrsStopper *>(p_user);
		stopper->service->shutdown();
		stopper->returned.set();
	}
};

} // namespace

TEST_CASE("[GaussianSplatting][PayloadReadService] shutdown wakes waiters, stops running reads and leaves nothing behind") {
	const uint64_t slice = PRS::read_slice_bytes;
	PrsFile file("shutdown", 3 * slice);
	PrsReadGate gate;
	gate.ticket = 1;
	gate.hold.set();

	PRS service;
	service._test_set_read_hook(PrsReadGate::hook, &gate);
	PRS::Config config;
	config.reader_count = 1;
	REQUIRE(service.start(config) == OK);

	CHECK(service.submit(file.path, 0, 3 * slice, 1) == PRS::SUBMIT_OK);
	CHECK(_prs_wait_until([&]() { return gate.entered.is_set(); }));
	CHECK(service.submit(file.path, 0, 16, 2) == PRS::SUBMIT_OK);

	PrsWaiter waiter;
	waiter.service = &service;
	Thread waiter_thread;
	waiter_thread.start(PrsWaiter::run, &waiter);
	OS::get_singleton()->delay_usec(20000); // Lets it block; a late start still returns false.

	PrsStopper stopper;
	stopper.service = &service;
	Thread stopper_thread;
	stopper_thread.start(PrsStopper::run, &stopper);

	// The waiter must leave while the read is still held, and shutdown must not finish
	// before the held read does.
	CHECK(_prs_wait_until([&]() { return waiter.returned.is_set(); }));
	CHECK_FALSE(waiter.got_completion.is_set());
	CHECK_FALSE(stopper.returned.is_set());

	gate.hold.clear();
	CHECK(_prs_wait_until([&]() { return stopper.returned.is_set(); }));
	stopper_thread.wait_to_finish();
	waiter_thread.wait_to_finish();

	PRS::Completion completion;
	const PRS::Stats stats = service.stats();
	CHECK(gate.ticket_slices.get() == 1); // Stopped at the slice boundary.
	CHECK(stats.discarded_on_shutdown == 2);
	CHECK(stats.completed_failed == 0); // A stop is not an I/O failure.
	CHECK(stats.bytes_in_flight == 0);
	CHECK(stats.open_handles == 0);
	CHECK(stats.reading == 0);
	CHECK(stats.queued == 0);
	CHECK(stats.ready == 0);
	CHECK(service.submit(file.path, 0, 16, 3) == PRS::SUBMIT_NOT_RUNNING);
	CHECK_FALSE(service.poll_completion(completion));
	CHECK_FALSE(service.wait_completion(completion));
	CHECK_FALSE(service.cancel(2));
	service.shutdown(); // Idempotent.

	// The destructor alone stops a loaded service, and a service that never started.
	for (uint32_t round = 0; round < 8; round++) {
		PRS loaded;
		REQUIRE(loaded.start(PRS::Config()) == OK);
		for (uint32_t i = 0; i < 16; i++) {
			CHECK(loaded.submit(file.path, 0, slice, i) == PRS::SUBMIT_OK);
		}
	}
	PRS never_started;
	CHECK(never_started.submit(file.path, 0, 16, 1) == PRS::SUBMIT_NOT_RUNNING);
}
