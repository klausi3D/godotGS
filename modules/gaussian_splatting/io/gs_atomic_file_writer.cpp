#include "gs_atomic_file_writer.h"

#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"

#include <atomic>

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

Error _gs_atomic_rename_temp(const String &p_temp_path, const String &p_final_path) {
	Ref<DirAccess> da = DirAccess::create_for_path(p_final_path);
	if (da.is_null()) {
		_gs_atomic_remove_temp(p_temp_path);
		return ERR_FILE_CANT_WRITE;
	}

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
