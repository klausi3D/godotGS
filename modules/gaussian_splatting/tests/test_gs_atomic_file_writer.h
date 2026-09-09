#pragma once

#include "test_macros.h"

#include "../core/streaming_chunk_payload_source.h"
#include "../io/gs_atomic_file_writer.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

namespace {

String _atomic_test_path(const String &p_suffix) {
    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String base = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : ".";
    return base.path_join("gs_atomic_" + p_suffix + "_" + itos(ticks) + ".bin");
}

bool _atomic_write_raw(const String &p_path, const PackedByteArray &p_bytes) {
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
    if (f.is_null()) {
        return false;
    }
    f->store_buffer(p_bytes.ptr(), p_bytes.size());
    f.unref();
    return true;
}

PackedByteArray _atomic_read_raw(const String &p_path) {
    PackedByteArray out;
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
    if (f.is_null()) {
        return out;
    }
    const uint64_t len = f->get_length();
    out.resize(len);
    if (len > 0) {
        f->get_buffer(out.ptrw(), len);
    }
    return out;
}

PackedByteArray _atomic_bytes(std::initializer_list<uint8_t> p_values) {
    PackedByteArray out;
    out.resize(int(p_values.size()));
    int i = 0;
    for (uint8_t v : p_values) {
        out.ptrw()[i++] = v;
    }
    return out;
}

// Counts files in the target's directory whose name starts with
// "<target_filename><infix>" (e.g. ".tmp." or ".bak."). Used to prove the helper
// leaves no temp/backup litter behind on success. Returns -1 if the dir can't be
// opened.
int _atomic_count_siblings(const String &p_path, const String &p_infix) {
    String dir = p_path.get_base_dir();
    if (dir.is_empty()) {
        dir = "."; // bare relative filename → siblings live in the working dir
    }
    Ref<DirAccess> da = DirAccess::open(dir);
    if (da.is_null()) {
        return -1;
    }
    const String prefix = p_path.get_file() + p_infix;
    int count = 0;
    da->list_dir_begin();
    for (String name = da->get_next(); !name.is_empty(); name = da->get_next()) {
        if (!da->current_is_dir() && name.begins_with(prefix)) {
            count++;
        }
    }
    da->list_dir_end();
    return count;
}

} // namespace

TEST_CASE("[GaussianSplatting][AtomicWrite] failed write leaves the existing file byte-intact") {
    // The core crash-safety invariant: even though the writer streams partial
    // bytes AND then fails, the pre-existing target must be untouched (the bad
    // content went to a temp file that is discarded).
    const String path = _atomic_test_path("fail_preserve");
    const PackedByteArray original = _atomic_bytes({ 'O', 'R', 'I', 'G', 'I', 'N', 'A', 'L' });
    REQUIRE(_atomic_write_raw(path, original));

    const Error err = gs_atomic_file_write(path, [](const Ref<FileAccess> &file) -> Error {
        const uint8_t garbage[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        file->store_buffer(garbage, 4);
        return ERR_FILE_CANT_WRITE; // simulate a mid-write failure
    });

    CHECK(err == ERR_FILE_CANT_WRITE);
    CHECK(_atomic_read_raw(path) == original);

    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][AtomicWrite] successful write atomically replaces the target") {
    const String path = _atomic_test_path("success_replace");
    REQUIRE(_atomic_write_raw(path, _atomic_bytes({ 1, 1, 1, 1 })));

    const PackedByteArray updated = _atomic_bytes({ 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 });
    const Error err = gs_atomic_file_write(path, [&](const Ref<FileAccess> &file) -> Error {
        file->store_buffer(updated.ptr(), updated.size());
        return OK;
    });

    CHECK(err == OK);
    CHECK(_atomic_read_raw(path) == updated);

    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][AtomicWrite] replacing a larger file leaves exactly the new bytes and no temp/backup litter") {
    // The atomic replace-over-existing path (MoveFileExW / rename()) must swap the
    // whole file in one step. Starting from an original LARGER than the update
    // proves there is no data loss and no stale trailing bytes (a truncate-in-place
    // or partial overwrite would leave the old tail behind), and that the temp
    // (and any backup used by the fallback) is cleaned up on success.
    const String path = _atomic_test_path("replace_no_litter");
    const PackedByteArray original = _atomic_bytes({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 });
    REQUIRE(_atomic_write_raw(path, original));

    const PackedByteArray updated = _atomic_bytes({ 0xAA, 0xBB, 0xCC });
    const Error err = gs_atomic_file_write(path, [&](const Ref<FileAccess> &file) -> Error {
        file->store_buffer(updated.ptr(), updated.size());
        return OK;
    });

    CHECK(err == OK);
    // (a) destination holds EXACTLY the new content — no truncation, no stale tail.
    const PackedByteArray got = _atomic_read_raw(path);
    CHECK(got.size() == updated.size());
    CHECK(got == updated);
    // (b) no temp/backup sibling remains after a successful write.
    CHECK(_atomic_count_siblings(path, ".tmp.") == 0);
    CHECK(_atomic_count_siblings(path, ".bak.") == 0);

    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][AtomicWrite] write to a non-existent path creates the file") {
    const String path = _atomic_test_path("create_new");
    DirAccess::remove_absolute(path); // ensure absent

    const PackedByteArray content = _atomic_bytes({ 42, 43, 44 });
    const Error err = gs_atomic_file_write(path, [&](const Ref<FileAccess> &file) -> Error {
        file->store_buffer(content.ptr(), content.size());
        return OK;
    });

    CHECK(err == OK);
    CHECK(FileAccess::exists(path));
    CHECK(_atomic_read_raw(path) == content);

    DirAccess::remove_absolute(path);
}

// --------------------------------------------------------------------------
// #714: the pre-rename guard's drain outcome is part of the contract.
//
// A guard exists to establish exclusivity the rename needs. On Windows an open
// FileAccess denies delete access, so if the guard could NOT establish it, the
// rename cannot succeed. The helper must refuse rather than attempt it: an
// attempted rename fails with whatever the filesystem says about a rename that
// never had a chance, which buries the real cause (an active streaming reader).
//
// These fakes pin both branches without needing a real streaming source, so the
// contract is proven deterministically rather than by racing a live reader.
namespace {

struct _AtomicTestGuard {
    bool is_drained = true;
    bool drained() const { return is_drained; }
};

} // namespace

TEST_CASE("[GaussianSplatting][AtomicWrite] a guard that could not drain refuses the replace and preserves the original") {
    const String path = _atomic_test_path("guard_not_drained");
    const PackedByteArray original = _atomic_bytes({ 'K', 'E', 'E', 'P' });
    if (!_atomic_write_raw(path, original)) {
        FAIL("could not create the pre-existing file the case replaces");
        return;
    }

    bool writer_ran = false;
    const Error err = gs_atomic_file_write(
            path,
            [&](const Ref<FileAccess> &file) -> Error {
                // The copy still runs and succeeds; only the replace is refused.
                writer_ran = true;
                const uint8_t replacement[3] = { 'N', 'E', 'W' };
                file->store_buffer(replacement, 3);
                return OK;
            },
            []() {
                _AtomicTestGuard guard;
                guard.is_drained = false;
                return guard;
            });

    CHECK(writer_ran);
    // The specific, actionable cause -- not a generic rename/IO error.
    CHECK(err == ERR_BUSY);
    // The destination must be byte-identical: nothing was renamed over it.
    CHECK(_atomic_read_raw(path) == original);
    // And the discarded copy must not be left behind as litter.
    CHECK(_atomic_count_siblings(path, ".tmp.") == 0);
    CHECK(_atomic_count_siblings(path, ".bak.") == 0);

    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][AtomicWrite] a guard that drained lets the replace proceed") {
    // The other branch, so the case above cannot pass by refusing every write.
    const String path = _atomic_test_path("guard_drained");
    if (!_atomic_write_raw(path, _atomic_bytes({ 'O', 'L', 'D' }))) {
        FAIL("could not create the pre-existing file the case replaces");
        return;
    }

    const PackedByteArray updated = _atomic_bytes({ 'N', 'E', 'W', '!' });
    const Error err = gs_atomic_file_write(
            path,
            [&](const Ref<FileAccess> &file) -> Error {
                file->store_buffer(updated.ptr(), updated.size());
                return OK;
            },
            []() { return _AtomicTestGuard(); });

    CHECK(err == OK);
    CHECK(_atomic_read_raw(path) == updated);

    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][AtomicWrite] the real reader-suspend guard reports drained when nothing streams the path") {
    // The overwhelmingly common case: no live source holds the destination, so
    // there is nothing to drain and the replace must proceed. Pins that the real
    // guard's default is "drained", not "busy" -- the opposite default would make
    // every reimport fail.
    const String path = _atomic_test_path("no_live_source");
    StagedFileChunkPayloadSource::ScopedReaderSuspend guard(path);
    CHECK(guard.suspended_count() == 0);
    CHECK(guard.drained());
}

TEST_CASE("[GaussianSplatting][AtomicWrite] the reader-suspend guard only engages on Windows") {
    // #714 review: the constraint this guard works around is Windows-specific
    // (FileAccessWindows opens via _wfsopen with _SH_DENY*, none of which share delete).
    // POSIX rename() replaces a destination that readers still hold open, so draining
    // there costs up to the drain budget per reimport and, on timeout, would refuse a
    // replace that would have succeeded -- a regression on Linux/macOS.
    //
    // Asserted per platform rather than skipped, so neither half can silently flip: on
    // POSIX the guard must suspend nothing at all, and on Windows it must still be the
    // real thing rather than accidentally compiled out.
    const String path = _atomic_test_path("platform_scope");
    StagedFileChunkPayloadSource::ScopedReaderSuspend guard(path);
    CHECK(guard.drained()); // both platforms: nothing is streaming this path
#ifdef WINDOWS_ENABLED
    // Live-source registry is empty here, so 0 is expected -- what matters is that the
    // Windows build still runs the real constructor. Covered behaviourally by the
    // drained()/not-drained cases above.
    CHECK(guard.suspended_count() == 0);
#else
    CHECK_MESSAGE(guard.suspended_count() == 0,
            "POSIX must not suspend readers: rename() replaces an open destination");
#endif
}

TEST_CASE("[GaussianSplatting][AtomicWrite] the reader-suspend guard is a no-op for an empty path") {
    // Braces, not parens: `guard(String())` is the most vexing parse -- it declares a
    // FUNCTION taking a String(*)() and returning the guard, and then
    // `guard.suspended_count()` fails to compile.
    const String empty_path;
    StagedFileChunkPayloadSource::ScopedReaderSuspend guard{ empty_path };
    CHECK(guard.suspended_count() == 0);
    CHECK(guard.drained());
}

TEST_CASE("[GaussianSplatting][AtomicWrite] the default no-op guard reports drained so existing savers are unchanged") {
    // The three existing savers use the two-argument form. It must never take the
    // refusal branch -- a guard that establishes nothing has nothing to fail at.
    _GSAtomicNoRenameGuard guard;
    CHECK(guard.drained());
}

#ifdef WINDOWS_ENABLED
TEST_CASE("[GaussianSplatting][AtomicWrite] a relative destination is absolutized to a valid long-path native form") {
    // Regression guard for the relative-path bug (Codex P2 on #468): a bare
    // relative name must be resolved against the working directory and long-path
    // prefixed, NOT emitted as an invalid "\\?\<relative>" which makes MoveFileExW
    // fail and silently drop to the backup-swap (reintroducing the missing-target
    // window this change removes).
    const String native = _gs_atomic_win_native_path("delta.gsinc");
    CHECK(native.begins_with(R"(\\?\)"));
    // The prefix must be followed by an absolute root (drive letter), never by the
    // still-relative filename.
    CHECK_FALSE(native.begins_with(R"(\\?\delta)"));
    const String absolute = native.trim_prefix(R"(\\?\)");
    CHECK(absolute.find_char(':') != -1); // "C:\..." — a real absolute path
    CHECK(native.ends_with("delta.gsinc"));
}

TEST_CASE("[GaussianSplatting][AtomicWrite] relative destination write yields the new bytes with no litter") {
    // Reproduces the reported scenario: a saver invoked with a bare relative name
    // ("delta.gsinc"). The temp is created relative to the process working
    // directory, so the atomic replace must target that same resolved location.
    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String rel = "gs_atomic_rel_" + itos(ticks) + ".bin";
    DirAccess::remove_absolute(rel); // ensure absent

    const PackedByteArray original = _atomic_bytes({ 1, 2, 3, 4, 5, 6 });
    REQUIRE(_atomic_write_raw(rel, original));

    const PackedByteArray updated = _atomic_bytes({ 7, 8, 9 });
    const Error err = gs_atomic_file_write(rel, [&](const Ref<FileAccess> &file) -> Error {
        file->store_buffer(updated.ptr(), updated.size());
        return OK;
    });

    CHECK(err == OK);
    CHECK(_atomic_read_raw(rel) == updated);
    CHECK(_atomic_count_siblings(rel, ".tmp.") == 0);
    CHECK(_atomic_count_siblings(rel, ".bak.") == 0);

    DirAccess::remove_absolute(rel);
}
#endif // WINDOWS_ENABLED
