#pragma once

#include "test_macros.h"

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
