#include "gs_atomic_file_transaction.h"

#include "gs_atomic_file_writer.h"

#include "core/config/project_settings.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

// Native primitives. Included AFTER the engine headers so <windows.h> macro
// pollution (ERROR/min/max) cannot reach them; same precedent as
// gs_atomic_file_writer.cpp.
#if defined(WINDOWS_ENABLED)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

// Largest single native write. WriteFile takes a DWORD byte count, and bounded
// slices keep every syscall well inside the 32-bit range on all platforms.
constexpr uint64_t kGSTxnMaxWriteSlice = uint64_t(64) << 20u;

#if defined(WINDOWS_ENABLED)
Error _gs_txn_map_os_error(DWORD p_code, Error p_fallback) {
	switch (p_code) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
		case ERROR_INVALID_NAME:
		case ERROR_BAD_PATHNAME:
		case ERROR_INVALID_DRIVE:
		case ERROR_DIRECTORY:
			return ERR_FILE_BAD_PATH;
		case ERROR_ACCESS_DENIED:
		case ERROR_WRITE_PROTECT:
			return ERR_FILE_NO_PERMISSION;
		case ERROR_SHARING_VIOLATION:
		case ERROR_LOCK_VIOLATION:
			return ERR_FILE_ALREADY_IN_USE;
		case ERROR_FILE_EXISTS:
		case ERROR_ALREADY_EXISTS:
			return ERR_ALREADY_EXISTS;
		default:
			return p_fallback;
	}
}
#else
Error _gs_txn_map_os_error(int p_code, Error p_fallback) {
	switch (p_code) {
		case ENOENT:
		case ENOTDIR:
		case ENAMETOOLONG:
		case EISDIR:
			return ERR_FILE_BAD_PATH;
		case EACCES:
		case EPERM:
		case EROFS:
			return ERR_FILE_NO_PERMISSION;
		case EBUSY:
		case ETXTBSY:
			return ERR_FILE_ALREADY_IN_USE;
		case EEXIST:
			return ERR_ALREADY_EXISTS;
		default:
			return p_fallback;
	}
}

String _gs_txn_globalize(const String &p_path) {
	if (ProjectSettings::get_singleton()) {
		return ProjectSettings::get_singleton()->globalize_path(p_path);
	}
	return p_path;
}

// Flushes file data to stable storage. fdatasync is sufficient (the ADR's Linux
// sequence): the size change it implies is metadata fdatasync must persist.
// Apple's fsync does not flush the drive cache; F_FULLFSYNC does, and fsync is
// the fallback where the filesystem refuses it.
int _gs_txn_sync_fd(int p_fd) {
#if defined(__APPLE__)
	if (::fcntl(p_fd, F_FULLFSYNC) == 0) {
		return 0;
	}
	return ::fsync(p_fd);
#else
	return ::fdatasync(p_fd);
#endif
}
#endif

} // namespace

GSAtomicFileTransaction::~GSAtomicFileTransaction() {
	abort();
}

Error GSAtomicFileTransaction::_fail(Error p_error, const String &p_stage, int64_t p_os_code) {
	error = p_error;
	error_message = vformat("GSAtomicFileTransaction: %s failed for '%s' (os error %d).", p_stage, final_path, p_os_code);
	_close_native();
	_remove_temp_native();
	state = State::FAILED;
	return p_error;
}

void GSAtomicFileTransaction::_close_native() {
#if defined(WINDOWS_ENABLED)
	if (native_handle != nullptr) {
		CloseHandle((HANDLE)native_handle);
		native_handle = nullptr;
	}
#else
	if (native_fd >= 0) {
		::close(native_fd);
		native_fd = -1;
	}
#endif
}

void GSAtomicFileTransaction::_remove_temp_native() {
	// Only ever the temp this transaction created (CREATE_NEW / O_EXCL), and only
	// while it still is a temp: once the replace succeeded the path is cleared.
	if (native_temp_path.is_empty()) {
		return;
	}
#if defined(WINDOWS_ENABLED)
	const Char16String w = native_temp_path.utf16();
	DeleteFileW((LPCWSTR)w.get_data());
#else
	const CharString c = native_temp_path.utf8();
	::unlink(c.get_data());
#endif
	native_temp_path = String();
}

Error GSAtomicFileTransaction::begin(const String &p_final_path) {
	if (state == State::OPEN) {
		return ERR_ALREADY_IN_USE;
	}
	state = State::IDLE;
	size = 0;
	error = OK;
	error_message = String();
	final_path = p_final_path;
	temp_path = String();
	native_final_path = String();
	native_temp_path = String();

	if (p_final_path.is_empty()) {
		return _fail(ERR_INVALID_PARAMETER, "begin (empty path)", 0);
	}
	temp_path = _gs_atomic_temp_path(p_final_path);

#if defined(WINDOWS_ENABLED)
	native_final_path = _gs_atomic_win_native_path(p_final_path);
	const String native_temp = _gs_atomic_win_native_path(temp_path);
	const Char16String w = native_temp.utf16();
	// CREATE_NEW: never truncate or adopt an existing file. FILE_SHARE_READ lets a
	// reader inspect the temp while it grows; no other writer can open it.
	// GENERIC_READ as well, for read_at() (scratch/spill read-back on the same handle).
	HANDLE h = CreateFileW((LPCWSTR)w.get_data(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		const DWORD code = GetLastError();
		return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_OPEN), "CreateFileW(temp)", int64_t(code));
	}
	native_handle = (void *)h;
	native_temp_path = native_temp;
#else
	native_final_path = _gs_txn_globalize(p_final_path);
	const String native_temp = _gs_txn_globalize(temp_path);
	const CharString c = native_temp.utf8();
	int flags = O_RDWR | O_CREAT | O_EXCL; // O_RDWR for read_at()
#if defined(O_CLOEXEC)
	flags |= O_CLOEXEC;
#endif
	int fd = -1;
	do {
		fd = ::open(c.get_data(), flags, 0666);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0) {
		const int code = errno;
		return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_OPEN), "open(temp)", int64_t(code));
	}
	native_fd = fd;
	native_temp_path = native_temp;
#endif
	state = State::OPEN;
	return OK;
}

Error GSAtomicFileTransaction::_write_native(uint64_t p_offset, const uint8_t *p_data, uint64_t p_size) {
	uint64_t done = 0;
	while (done < p_size) {
		const uint64_t want = MIN(kGSTxnMaxWriteSlice, p_size - done);
		const uint64_t at = p_offset + done;
#if defined(WINDOWS_ENABLED)
		OVERLAPPED ov = {};
		ov.Offset = DWORD(at & 0xFFFFFFFFu);
		ov.OffsetHigh = DWORD(at >> 32u);
		DWORD wrote = 0;
		if (!WriteFile((HANDLE)native_handle, p_data + done, DWORD(want), &wrote, &ov)) {
			const DWORD code = GetLastError();
			return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_WRITE), "WriteFile(temp)", int64_t(code));
		}
		if (wrote == 0) {
			return _fail(ERR_FILE_CANT_WRITE, "WriteFile(temp) wrote 0 bytes", 0);
		}
#else
		ssize_t wrote = -1;
		do {
			wrote = ::pwrite(native_fd, p_data + done, size_t(want), off_t(at));
		} while (wrote < 0 && errno == EINTR);
		if (wrote < 0) {
			const int code = errno;
			return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_WRITE), "pwrite(temp)", int64_t(code));
		}
		if (wrote == 0) {
			return _fail(ERR_FILE_CANT_WRITE, "pwrite(temp) wrote 0 bytes", 0);
		}
#endif
		done += uint64_t(wrote);
	}
	return OK;
}

Error GSAtomicFileTransaction::append(const uint8_t *p_data, uint64_t p_size) {
	if (state != State::OPEN) {
		return state == State::FAILED ? error : ERR_UNCONFIGURED;
	}
	if (p_size == 0) {
		return OK;
	}
	if (p_data == nullptr || p_size > UINT64_MAX - size) {
		return ERR_INVALID_PARAMETER;
	}
	const Error err = _write_native(size, p_data, p_size);
	if (err != OK) {
		return err;
	}
	size += p_size;
	return OK;
}

Error GSAtomicFileTransaction::write_at(uint64_t p_offset, const uint8_t *p_data, uint64_t p_size) {
	if (state != State::OPEN) {
		return state == State::FAILED ? error : ERR_UNCONFIGURED;
	}
	if (p_offset > size || p_size > size - p_offset) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_size == 0) {
		return OK;
	}
	if (p_data == nullptr) {
		return ERR_INVALID_PARAMETER;
	}
	return _write_native(p_offset, p_data, p_size);
}

Error GSAtomicFileTransaction::read_at(uint64_t p_offset, uint8_t *r_data, uint64_t p_size) {
	if (state != State::OPEN) {
		return state == State::FAILED ? error : ERR_UNCONFIGURED;
	}
	if (p_offset > size || p_size > size - p_offset) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_size == 0) {
		return OK;
	}
	if (r_data == nullptr) {
		return ERR_INVALID_PARAMETER;
	}
	uint64_t done = 0;
	while (done < p_size) {
		const uint64_t want = MIN(kGSTxnMaxWriteSlice, p_size - done);
		const uint64_t at = p_offset + done;
#if defined(WINDOWS_ENABLED)
		OVERLAPPED ov = {};
		ov.Offset = DWORD(at & 0xFFFFFFFFu);
		ov.OffsetHigh = DWORD(at >> 32u);
		DWORD got = 0;
		if (!ReadFile((HANDLE)native_handle, r_data + done, DWORD(want), &got, &ov)) {
			const DWORD code = GetLastError();
			return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_READ), "ReadFile(temp)", int64_t(code));
		}
		if (got == 0) {
			return _fail(ERR_FILE_CANT_READ, "ReadFile(temp) read 0 bytes", 0);
		}
#else
		ssize_t got = -1;
		do {
			got = ::pread(native_fd, r_data + done, size_t(want), off_t(at));
		} while (got < 0 && errno == EINTR);
		if (got < 0) {
			const int code = errno;
			return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_READ), "pread(temp)", int64_t(code));
		}
		if (got == 0) {
			return _fail(ERR_FILE_CANT_READ, "pread(temp) read 0 bytes", 0);
		}
#endif
		done += uint64_t(got);
	}
	return OK;
}

Error GSAtomicFileTransaction::flush_to_storage() {
	if (state != State::OPEN) {
		return state == State::FAILED ? error : ERR_UNCONFIGURED;
	}
#if defined(WINDOWS_ENABLED)
	if (!FlushFileBuffers((HANDLE)native_handle)) {
		const DWORD code = GetLastError();
		return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_WRITE), "FlushFileBuffers(temp)", int64_t(code));
	}
#else
	if (_gs_txn_sync_fd(native_fd) != 0) {
		const int code = errno;
		return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_WRITE), "fdatasync(temp)", int64_t(code));
	}
#endif
	return OK;
}

Error GSAtomicFileTransaction::commit() {
	if (state != State::OPEN) {
		return state == State::FAILED ? error : ERR_UNCONFIGURED;
	}
#if defined(WINDOWS_ENABLED)
	if (!FlushFileBuffers((HANDLE)native_handle)) {
		const DWORD code = GetLastError();
		return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_WRITE), "FlushFileBuffers(temp)", int64_t(code));
	}
	const HANDLE h = (HANDLE)native_handle;
	native_handle = nullptr;
	if (!CloseHandle(h)) {
		const DWORD code = GetLastError();
		return _fail(ERR_FILE_CANT_WRITE, "CloseHandle(temp)", int64_t(code));
	}
	const Char16String src = native_temp_path.utf16();
	const Char16String dst = native_final_path.utf16();
	if (!MoveFileExW((LPCWSTR)src.get_data(), (LPCWSTR)dst.get_data(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		const DWORD code = GetLastError();
		return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_WRITE), "MoveFileExW(temp -> final)", int64_t(code));
	}
	// The temp is now the final file; nothing left to clean up.
	native_temp_path = String();
#else
	if (_gs_txn_sync_fd(native_fd) != 0) {
		const int code = errno;
		return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_WRITE), "fdatasync(temp)", int64_t(code));
	}
	const int fd = native_fd;
	native_fd = -1;
	if (::close(fd) != 0) {
		// Do not retry close() (POSIX leaves the descriptor state unspecified on
		// EINTR); a failed close may have lost data, so refuse the replace.
		const int code = errno;
		return _fail(ERR_FILE_CANT_WRITE, "close(temp)", int64_t(code));
	}
	const CharString src = native_temp_path.utf8();
	const CharString dst = native_final_path.utf8();
	if (::rename(src.get_data(), dst.get_data()) != 0) {
		const int code = errno;
		return _fail(_gs_txn_map_os_error(code, ERR_FILE_CANT_WRITE), "rename(temp -> final)", int64_t(code));
	}
	native_temp_path = String();

	// Persist the directory entry. The replace has already happened, so a failure
	// here cannot be undone: report it, the new file stays in place.
	String dir = native_final_path.get_base_dir();
	if (dir.is_empty()) {
		dir = ".";
	}
	const CharString dir_c = dir.utf8();
	int dir_flags = O_RDONLY;
#if defined(O_DIRECTORY)
	dir_flags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
	dir_flags |= O_CLOEXEC;
#endif
	int dir_fd = -1;
	do {
		dir_fd = ::open(dir_c.get_data(), dir_flags);
	} while (dir_fd < 0 && errno == EINTR);
	if (dir_fd < 0) {
		const int code = errno;
		return _fail(ERR_FILE_CANT_WRITE,
				"open(directory) after the replace; the new file is in place but its directory entry may not be durable",
				int64_t(code));
	}
	const int dir_sync = ::fsync(dir_fd);
	const int dir_code = errno;
	::close(dir_fd);
	if (dir_sync != 0) {
		return _fail(ERR_FILE_CANT_WRITE,
				"fsync(directory) after the replace; the new file is in place but its directory entry may not be durable",
				int64_t(dir_code));
	}
#endif
	state = State::COMMITTED;
	return OK;
}

void GSAtomicFileTransaction::abort() {
	if (state != State::OPEN) {
		return;
	}
	_close_native();
	_remove_temp_native();
	state = State::ABORTED;
}
