#pragma once

#include "core/error/error_list.h"
#include "core/string/ustring.h"

#include <cstdint>

// Long-lived, durable, crash-atomic file write on a NATIVE OS handle
// (docs/architecture/adr-beyond-vram-worlds.md §4.3, invariants W1/W9; BV-02).
//
// This is the durability layer the streaming world writer (BV-03a onward)
// builds on. Unlike `gs_atomic_file_write` (gs_atomic_file_writer.h), which
// streams through Godot `FileAccess` inside one call, this transaction stays open
// across many appends and does NOT use `FileAccess`: FileAccess adds its own
// backup temp, its flush is CRT-only (no fsync), and on Windows it opens
// `_SH_DENYRW`, which blocks a second handle. Per the ADR:
//
//   - Windows: `CreateFileW` (share-read, CREATE_NEW) on a sibling temp,
//     `WriteFile`, `FlushFileBuffers` on the same handle, `CloseHandle`, then
//     `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
//     Windows has no directory fsync.
//   - POSIX: `open(O_CREAT | O_EXCL)` on a sibling temp, `pwrite`, `fdatasync`
//     (`F_FULLFSYNC` on Apple), `close`, `rename`, then fsync the directory.
//
// Contract:
//   - Nothing exists at the final path, and a pre-existing final file is not
//     touched, until `commit()` performs the single atomic replace.
//   - `abort()`, the destructor, and every failure path close the handle and
//     remove the temp; the final path is never opened, written or removed by
//     them. (A process crash can leave the temp behind; it never replaces the
//     target.)
//   - There is no backup-swap fallback: if the atomic replace fails, `commit()`
//     reports the error, removes the temp and leaves the target as it was.
//   - Writes only grow the file or rewrite bytes already written (`write_at`
//     with `offset + size <= get_size()`, e.g. a final header rewrite, W2); no
//     write can leave a hole.
//   - Any failure poisons the transaction: it is cleaned up immediately, the
//     error stays readable through `get_error()` / `get_error_message()`, and
//     further calls return that error until the next `begin()`.
//
// Not thread-safe: one transaction is driven by one thread at a time.
class GSAtomicFileTransaction {
public:
	enum class State : uint8_t {
		IDLE, // never begun, or ready for a new begin()
		OPEN, // temp exists, writes accepted
		COMMITTED, // the final path names the new file
		ABORTED, // aborted by the caller or the destructor; no temp
		FAILED, // a step failed; cleaned up; see get_error()
	};

	GSAtomicFileTransaction() = default;
	~GSAtomicFileTransaction();

	GSAtomicFileTransaction(const GSAtomicFileTransaction &) = delete;
	GSAtomicFileTransaction &operator=(const GSAtomicFileTransaction &) = delete;
	GSAtomicFileTransaction(GSAtomicFileTransaction &&) = delete;
	GSAtomicFileTransaction &operator=(GSAtomicFileTransaction &&) = delete;

	// Creates a fresh sibling temp next to `p_final_path` (Godot VFS paths such as
	// res:// / user:// are globalized). Refused with ERR_ALREADY_IN_USE while a
	// transaction is OPEN.
	Error begin(const String &p_final_path);

	// Appends at the current end.
	Error append(const uint8_t *p_data, uint64_t p_size);

	// Rewrites already-written bytes; `p_offset + p_size` must not exceed
	// get_size(). ERR_INVALID_PARAMETER otherwise (the transaction is NOT
	// poisoned by a rejected argument; nothing was written).
	Error write_at(uint64_t p_offset, const uint8_t *p_data, uint64_t p_size);

	// Flushes the data to stable storage, closes the handle and atomically
	// replaces the final path. On POSIX the containing directory is then fsynced;
	// if only that last step fails the new file is already in place, the state is
	// FAILED, and the error says so.
	Error commit();

	// Closes the handle and removes the temp. Never touches the final path.
	// Idempotent; a no-op unless OPEN.
	void abort();

	State get_state() const { return state; }
	bool is_open() const { return state == State::OPEN; }
	uint64_t get_size() const { return size; }
	const String &get_final_path() const { return final_path; }
	// The Godot-form temp path (for diagnostics and tests). Empty before begin().
	const String &get_temp_path() const { return temp_path; }
	Error get_error() const { return error; }
	const String &get_error_message() const { return error_message; }

private:
	Error _fail(Error p_error, const String &p_stage, int64_t p_os_code);
	Error _write_native(uint64_t p_offset, const uint8_t *p_data, uint64_t p_size);
	void _close_native();
	void _remove_temp_native();

	State state = State::IDLE;
	uint64_t size = 0;
	String final_path;
	String temp_path;
	String native_final_path;
	String native_temp_path;
	Error error = OK;
	String error_message;
#if defined(WINDOWS_ENABLED)
	void *native_handle = nullptr; // HANDLE; nullptr when closed
#else
	int native_fd = -1;
#endif
};
