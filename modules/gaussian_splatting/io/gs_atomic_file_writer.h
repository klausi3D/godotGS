#pragma once

#include "core/error/error_list.h"
#include "core/io/file_access.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"

// Atomic file write for the persistence/import savers.
//
// The savers historically did `FileAccess::open(path, WRITE)`, which truncates
// the destination immediately and then streams content. A crash or write error
// mid-save leaves a truncated/empty file, destroying the previous good copy
// (worst case: the baseline the incremental-delta system depends on).
//
// `gs_atomic_file_write` instead streams the content to a sibling temp file and
// only moves it into place once the whole write has succeeded and the file has
// been closed. The final move is a TRUE atomic replace-over-existing
// (`MoveFileExW` on Windows, `rename()` on POSIX), so a concurrent reader always
// sees either the old or the new file and never a missing target. On any
// writer/rename failure the pre-existing destination is left byte-intact, and the
// temp is cleaned up.
//
// Caveats (see docs/architecture/adr-import-input-hardening.md):
//   - The atomic OS replace is the primary path and needs no backup. If it is
//     unavailable or errors, the helper falls back to a backup-swap: it moves any
//     existing target aside first (so each `DirAccess::rename` targets a
//     non-existent path, avoiding Godot's remove-then-move Windows data-loss
//     window) and restores it if the final move fails. The fallback's only
//     residual window is a crash BETWEEN the two moves, and even then no data is
//     lost: both the backup (old contents) and the temp (new contents) remain on
//     disk for recovery.
//   - `FileAccess::flush()` is a CRT-level flush AND Godot ignores its return
//     value, so a flush-only failure is not surfaced here. Write-completion is
//     therefore validated by the caller's per-write `get_error()` checks (the
//     writer lambda must return non-OK on any failed store); this helper's
//     flush()+get_error() is a secondary check. A true fsync (power-loss
//     durability) would need an upstream engine primitive.

// Internals (defined in gs_atomic_file_writer.cpp). Not part of the public API;
// call gs_atomic_file_write below.
Ref<FileAccess> _gs_atomic_open_temp(const String &p_final_path, String &r_temp_path, Error *r_error);
void _gs_atomic_remove_temp(const String &p_temp_path);
Error _gs_atomic_rename_temp(const String &p_temp_path, const String &p_final_path);

#if defined(WINDOWS_ENABLED)
// Test hook (Windows only): the native Win32 path the atomic replace targets for
// `p_path`. Exposed so tests can assert a *relative* destination is absolutized to
// a valid long-path form instead of an invalid `\\?\`-prefixed relative string
// (which would make MoveFileExW fall back to the backup-swap). Not public API.
String _gs_atomic_win_native_path(const String &p_path);
#endif

// #714: default no-op pre-rename guard, so callers that need no exclusivity around
// the rename pay nothing and keep the two-argument call shape. It establishes
// nothing, so there is nothing that can fail to be established: always drained.
struct _GSAtomicNoRenameGuard {
	bool drained() const { return true; }
};

// Writes `p_final_path` atomically. `p_write` is invoked with the open temp file
// and must return OK on success; it must NOT retain a persistent Ref to the file
// beyond its own scope (transient Refs passed to helper functions are fine).
//
// `p_make_rename_guard` is invoked with no arguments immediately before the atomic
// rename, and whatever it returns is kept alive until the rename has finished. It
// exists for writers whose destination may be held open by other readers: on
// Windows the rename needs delete access, which an open FileAccess denies (#714).
// Scope it to the RENAME ONLY -- the copy above can take arbitrarily long, and any
// exclusivity held across it would stall readers for the size of the file.
//
// The guard must expose `bool drained() const`, reporting whether it actually
// established that exclusivity. When it returns false the rename is NOT attempted:
// its precondition is known to be violated, so it would fail and report whatever
// the filesystem says about a rename that never had a chance, burying the real
// cause. The temp is removed and ERR_BUSY is returned instead. A guard that
// establishes nothing returns true -- there is nothing for it to fail at.
template <typename TWriter, typename TRenameGuard>
Error gs_atomic_file_write(const String &p_final_path, TWriter &&p_write, TRenameGuard &&p_make_rename_guard) {
	String temp_path;
	Error open_err = OK;
	Ref<FileAccess> file = _gs_atomic_open_temp(p_final_path, temp_path, &open_err);
	if (file.is_null()) {
		return open_err != OK ? open_err : ERR_FILE_CANT_WRITE;
	}

	const Error write_err = p_write(file);
	if (write_err != OK) {
		file.unref();
		_gs_atomic_remove_temp(temp_path);
		return write_err;
	}

	file->flush();
	const Error io_err = file->get_error();
	// Drop the sole reference so the OS handle is closed BEFORE the rename;
	// Windows cannot rename over / move an open file.
	file.unref();
	if (io_err != OK) {
		_gs_atomic_remove_temp(temp_path);
		return io_err;
	}

	// Guaranteed copy elision (C++17) constructs the guard in place, so it needs no
	// copy or move constructor. It lives until this scope ends, i.e. across the
	// rename and nothing else.
	auto rename_guard = p_make_rename_guard();
	if (!rename_guard.drained()) {
		_gs_atomic_remove_temp(temp_path);
		return ERR_BUSY;
	}
	return _gs_atomic_rename_temp(temp_path, p_final_path);
}

// Two-argument form: no exclusivity around the rename. This is what the three
// existing savers use and their behaviour is unchanged.
template <typename TWriter>
Error gs_atomic_file_write(const String &p_final_path, TWriter &&p_write) {
	return gs_atomic_file_write(p_final_path, p_write, []() { return _GSAtomicNoRenameGuard(); });
}
