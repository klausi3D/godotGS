#include "gs_atomic_file_writer.h"

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"

#include <atomic>

// Platform primitives for a TRUE atomic replace-over-existing. Included AFTER the
// engine headers so <windows.h> macro pollution (ERROR/min/max) can't reach them.
#if defined(WINDOWS_ENABLED)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cstdio> // ::rename
#endif

// usec + a process-local monotonic counter, so temp/backup sibling names are
// unique even for two saves of the same target within the same microsecond. (Two
// separate processes saving the identical path concurrently is out of scope and
// unusual for editor asset writes.)
static String _gs_atomic_unique_suffix() {
	static std::atomic<uint64_t> s_counter{ 0 };
	const uint64_t usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	const uint64_t seq = s_counter.fetch_add(1, std::memory_order_relaxed);
	return itos(usec) + "." + itos(seq);
}

String _gs_atomic_temp_path(const String &p_final_path) {
	// Sibling path (same directory / VFS domain) so the rename stays within one
	// filesystem.
	return p_final_path + ".tmp." + _gs_atomic_unique_suffix();
}

Ref<FileAccess> _gs_atomic_open_temp(const String &p_final_path, String &r_temp_path, Error *r_error) {
	r_temp_path = _gs_atomic_temp_path(p_final_path);

	Error open_err = OK;
	Ref<FileAccess> file = FileAccess::open(r_temp_path, FileAccess::WRITE, &open_err);
	if (r_error) {
		*r_error = open_err;
	}
	return file;
}

void _gs_atomic_remove_temp(const String &p_temp_path) {
	if (p_temp_path.is_empty()) {
		return;
	}
	// Best-effort cleanup; a stray temp is harmless and never replaces the target.
	Ref<DirAccess> da = DirAccess::create_for_path(p_temp_path);
	if (da.is_valid()) {
		da->remove(p_temp_path);
	}
}

// Turns a Godot VFS path (res://, uid://, user://) into the native filesystem
// path the OS calls below understand. Absolute/`ACCESS_FILESYSTEM` paths pass
// through unchanged. Mirrors the public converter the rest of the engine uses.
static String _gs_atomic_globalize(const String &p_path) {
	if (ProjectSettings::get_singleton()) {
		return ProjectSettings::get_singleton()->globalize_path(p_path);
	}
	return p_path;
}

// Attempts a TRUE atomic replace-over-existing: p_final_path is made to name the
// new file in a single OS operation, with no intermediate state in which it is
// absent (unlike the backup-swap fallback). Returns OK only when the platform
// primitive actually moved the temp into place; any other return means "atomic
// replace unavailable or failed — fall back". On failure the temp is left in
// place for the fallback (both primitives are no-ops on failure).
static Error _gs_atomic_try_native_replace(const String &p_temp_path, const String &p_final_path) {
#if defined(WINDOWS_ENABLED)
	// Match DirAccessWindows::fix_path: native separators + long-path prefix so
	// paths beyond MAX_PATH still resolve. MoveFileExW is an atomic metadata swap.
	auto to_win_native = [](const String &p_path) -> String {
		String r = _gs_atomic_globalize(p_path).replace_char('/', '\\');
		if (!r.is_network_share_path() && !r.begins_with(R"(\\?\)")) {
			r = R"(\\?\)" + r;
		}
		return r;
	};
	const Char16String src_w = to_win_native(p_temp_path).utf16();
	const Char16String dst_w = to_win_native(p_final_path).utf16();
	if (MoveFileExW((LPCWSTR)src_w.get_data(), (LPCWSTR)dst_w.get_data(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
		return OK;
	}
	return FAILED;
#else
	// POSIX rename() atomically replaces an existing destination on the same
	// filesystem; the temp is a sibling of the target, so it always is.
	const CharString src = _gs_atomic_globalize(p_temp_path).utf8();
	const CharString dst = _gs_atomic_globalize(p_final_path).utf8();
	if (::rename(src.get_data(), dst.get_data()) == 0) {
		return OK;
	}
	return FAILED;
#endif
}

Error _gs_atomic_rename_temp(const String &p_temp_path, const String &p_final_path) {
	Ref<DirAccess> da = DirAccess::create_for_path(p_final_path);
	if (da.is_null()) {
		_gs_atomic_remove_temp(p_temp_path);
		return ERR_FILE_CANT_WRITE;
	}

	// Preferred path: a TRUE atomic replace-over-existing (MoveFileExW on Windows,
	// rename() on POSIX). It is windowless — a concurrent reader always sees either
	// the old or the new file, never a missing target — and needs no backup. On
	// success the temp has already been moved into place.
	if (_gs_atomic_try_native_replace(p_temp_path, p_final_path) == OK) {
		return OK;
	}

	// Fallback (atomic replace unavailable or errored): the backup-swap below.
	// It never loses data — both the backup (old contents) and the temp (new
	// contents) survive a crash — at the cost of a microsecond window in which
	// p_final_path is briefly absent. The temp is still present here because the
	// atomic attempt is a no-op on failure.
	//
	// Godot's DirAccess::rename is remove-then-move when the destination exists,
	// which on Windows deletes the original before the move and loses it if the
	// move then fails. Avoid that path entirely by moving any existing target
	// aside first, so BOTH renames below target a non-existent destination (a
	// single clean move each). On failure the original is restored from backup,
	// so the previous contents are never lost.
	const bool had_existing = FileAccess::exists(p_final_path);
	String backup_path;
	if (had_existing) {
		backup_path = p_final_path + ".bak." + _gs_atomic_unique_suffix();
		const Error backup_err = da->rename(p_final_path, backup_path);
		if (backup_err != OK) {
			// Could not move the original aside; leave it untouched.
			_gs_atomic_remove_temp(p_temp_path);
			return backup_err;
		}
	}

	const Error move_err = da->rename(p_temp_path, p_final_path);
	if (move_err != OK) {
		if (had_existing) {
			// Restore the original from backup so the target is never lost.
			const Error restore_err = da->rename(backup_path, p_final_path);
			if (restore_err != OK) {
				ERR_PRINT(vformat("gs_atomic_file_write: could not move the new file into place for '%s', "
								  "and failed to restore the original. Its previous contents are preserved at '%s'.",
						p_final_path, backup_path));
			}
		}
		_gs_atomic_remove_temp(p_temp_path);
		return move_err;
	}

	// The temp is now the target; drop the backup (best-effort).
	if (had_existing) {
		da->remove(backup_path);
	}
	return OK;
}
