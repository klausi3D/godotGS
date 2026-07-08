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
// been closed. On any writer/rename failure the pre-existing destination is left
// byte-intact (an existing target is moved to a backup first and restored if the
// final move fails), and the temp is cleaned up.
//
// Caveats (see docs/architecture/adr-import-input-hardening.md):
//   - Godot's `DirAccess::rename` is remove-then-move when the destination
//     exists (a Windows data-loss window). This helper sidesteps it by moving any
//     existing target aside first, so each rename targets a non-existent path.
//     The only residual window is a crash BETWEEN the two moves, and even then no
//     data is lost: both the backup (old contents) and the temp (new contents)
//     remain on disk for recovery.
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

// Writes `p_final_path` atomically. `p_write` is invoked with the open temp file
// and must return OK on success; it must NOT retain a persistent Ref to the file
// beyond its own scope (transient Refs passed to helper functions are fine).
template <typename TWriter>
Error gs_atomic_file_write(const String &p_final_path, TWriter &&p_write) {
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

	return _gs_atomic_rename_temp(temp_path, p_final_path);
}
