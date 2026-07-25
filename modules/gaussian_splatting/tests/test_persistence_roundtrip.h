#pragma once

#include "test_macros.h"
#include "../persistence/gaussian_scene_serializer.h"
#include "../persistence/incremental_saver.h"
#include "../core/gaussian_splat_world.h"
#include "../core/streaming_chunk_payload_source.h"

#include "core/io/file_access.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"

namespace {

String _make_persistence_fixture_path(const String &p_prefix, const String &p_suffix = ".gsf") {
    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String base_temp = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : ".";
    const String fixture_dir = base_temp.path_join("godotgs_persistence_fixtures");
    return fixture_dir.path_join(p_prefix + "_" + itos(ticks) + p_suffix);
}

bool _ensure_persistence_fixture_dir(const String &p_path) {
    const Error dir_err = DirAccess::make_dir_recursive_absolute(p_path.get_base_dir());
    return dir_err == OK || dir_err == ERR_ALREADY_EXISTS;
}

Ref<FileAccess> _open_persistence_fixture(const String &p_path, int p_mode_flags) {
    if (!_ensure_persistence_fixture_dir(p_path)) {
        return Ref<FileAccess>();
    }
    return FileAccess::open(p_path, p_mode_flags);
}

void _remove_persistence_fixture(const String &p_path) {
    DirAccess::remove_absolute(p_path);
}

bool _overwrite_scene_header_versions(const String &p_path, uint16_t p_version, uint16_t p_min_reader_version) {
    Ref<FileAccess> file = _open_persistence_fixture(p_path, FileAccess::READ_WRITE);
    if (file.is_null()) {
        return false;
    }

    const uint64_t payload_offset = sizeof(GaussianSplatting::ChunkHeader);
    if (file->get_length() < payload_offset + GaussianSplatting::SCENE_HEADER_PACKED_SIZE) {
        return false;
    }

    const uint64_t version_offset = payload_offset + sizeof(uint32_t);
    const uint64_t min_reader_offset = payload_offset + 56;
    file->seek(version_offset);
    file->store_16(p_version);
    file->seek(min_reader_offset);
    file->store_16(p_min_reader_version);
    return true;
}

bool _retag_first_metadata_chunk_as_unknown(const String &p_path, uint32_t p_unknown_chunk_type) {
    Ref<FileAccess> file = _open_persistence_fixture(p_path, FileAccess::READ_WRITE);
    if (file.is_null()) {
        return false;
    }

    const uint64_t file_length = file->get_length();
    file->seek(0);

    while (file->get_position() + uint64_t(sizeof(GaussianSplatting::ChunkHeader)) <= file_length) {
        const uint64_t chunk_start = file->get_position();
        const uint32_t chunk_type = file->get_32();
        const uint32_t chunk_size = file->get_32();
        file->get_32(); // checksum
        file->get_32(); // flags

        const uint64_t payload_offset = file->get_position();
        if (payload_offset > file_length || uint64_t(chunk_size) > file_length - payload_offset) {
            return false;
        }

        if (chunk_type == uint32_t(GaussianSplatting::ChunkType::METADATA)) {
            file->seek(chunk_start);
            file->store_32(p_unknown_chunk_type);
            return true;
        }

        if (chunk_type == uint32_t(GaussianSplatting::ChunkType::END_OF_FILE)) {
            break;
        }

        file->seek(payload_offset + uint64_t(chunk_size));
    }

    return false;
}

// Replaces the fixture's first METADATA chunk with a ZERO-SIZE chunk of an
// unknown type carrying p_checksum, rewriting the file so nothing else about it
// changes shape (Codex PR #718).
//
// Replace, never append: the chunk COUNT must stay equal to the header's
// `total_chunks` and the END_OF_FILE terminator must stay at the true end of
// the stream, or the loader's structural checks (#601/#700) reject the fixture
// before the checksum rule is ever consulted -- a test that would then pass for
// entirely the wrong reason. Nothing in SceneHeader records file size or chunk
// offsets, so dropping the payload invalidates no other field.
bool _replace_first_metadata_chunk_with_empty_unknown(
        const String &p_path, uint32_t p_unknown_chunk_type, uint32_t p_checksum) {
    PackedByteArray original;
    {
        Ref<FileAccess> file = _open_persistence_fixture(p_path, FileAccess::READ);
        if (file.is_null()) {
            return false;
        }
        original = file->get_buffer(file->get_length());
    }

    const uint64_t file_length = uint64_t(original.size());
    uint64_t cursor = 0;
    while (cursor + uint64_t(GaussianSplatting::GSF_CHUNK_HEADER_SIZE) <= file_length) {
        const uint8_t *p = original.ptr() + cursor;
        uint32_t chunk_type = 0;
        uint32_t chunk_size = 0;
        uint32_t chunk_flags = 0;
        memcpy(&chunk_type, p + 0, sizeof(uint32_t));
        memcpy(&chunk_size, p + 4, sizeof(uint32_t));
        memcpy(&chunk_flags, p + 12, sizeof(uint32_t));

        const uint64_t payload_offset = cursor + uint64_t(GaussianSplatting::GSF_CHUNK_HEADER_SIZE);
        if (payload_offset > file_length || uint64_t(chunk_size) > file_length - payload_offset) {
            return false;
        }

        if (chunk_type == uint32_t(GaussianSplatting::ChunkType::METADATA)) {
            PackedByteArray rewritten;
            rewritten.append_array(original.slice(0, int(cursor)));

            uint8_t header_bytes[GaussianSplatting::GSF_CHUNK_HEADER_SIZE];
            const uint32_t zero_size = 0;
            memcpy(header_bytes + 0, &p_unknown_chunk_type, sizeof(uint32_t));
            memcpy(header_bytes + 4, &zero_size, sizeof(uint32_t));
            memcpy(header_bytes + 8, &p_checksum, sizeof(uint32_t));
            memcpy(header_bytes + 12, &chunk_flags, sizeof(uint32_t));
            for (uint32_t i = 0; i < GaussianSplatting::GSF_CHUNK_HEADER_SIZE; ++i) {
                rewritten.push_back(header_bytes[i]);
            }

            const uint64_t tail_offset = payload_offset + uint64_t(chunk_size);
            rewritten.append_array(original.slice(int(tail_offset), original.size()));

            Ref<FileAccess> out = _open_persistence_fixture(p_path, FileAccess::WRITE);
            if (out.is_null()) {
                return false;
            }
            out->store_buffer(rewritten);
            return true;
        }

        if (chunk_type == uint32_t(GaussianSplatting::ChunkType::END_OF_FILE)) {
            break;
        }

        cursor = payload_offset + uint64_t(chunk_size);
    }

    return false;
}

bool _file_contains_chunk_type(const String &p_path, uint32_t p_chunk_type) {
    Ref<FileAccess> file = _open_persistence_fixture(p_path, FileAccess::READ);
    if (file.is_null()) {
        return false;
    }

    const uint64_t file_length = file->get_length();
    file->seek(0);

    while (file->get_position() + uint64_t(sizeof(GaussianSplatting::ChunkHeader)) <= file_length) {
        const uint32_t chunk_type = file->get_32();
        const uint32_t chunk_size = file->get_32();
        file->get_32(); // checksum
        file->get_32(); // flags

        const uint64_t payload_offset = file->get_position();
        if (payload_offset > file_length || uint64_t(chunk_size) > file_length - payload_offset) {
            return false;
        }

        if (chunk_type == p_chunk_type) {
            return true;
        }

        if (chunk_type == uint32_t(GaussianSplatting::ChunkType::END_OF_FILE)) {
            break;
        }

        file->seek(payload_offset + uint64_t(chunk_size));
    }

    return false;
}

// Rewrites the fixture with its last p_drop_bytes removed (FileAccess::WRITE
// truncates on open). Used to simulate a scene truncated before its terminator.
bool _truncate_fixture_tail(const String &p_path, uint64_t p_drop_bytes) {
    Ref<FileAccess> reader = FileAccess::open(p_path, FileAccess::READ);
    if (reader.is_null()) {
        return false;
    }
    const uint64_t length = reader->get_length();
    if (length < p_drop_bytes) {
        return false;
    }
    const uint64_t keep = length - p_drop_bytes;
    PackedByteArray head = reader->get_buffer(keep);
    reader.unref();
    if (uint64_t(head.size()) != keep) {
        return false;
    }
    Ref<FileAccess> writer = FileAccess::open(p_path, FileAccess::WRITE);
    if (writer.is_null()) {
        return false;
    }
    writer->store_buffer(head);
    return true;
}

// #700: rewrites the END_OF_FILE chunk header's declared size in place, without
// appending anything. Models a hand-crafted terminator that claims a payload the
// writer never emits.
bool _set_eof_chunk_declared_size(const String &p_path, uint32_t p_declared_size) {
    Ref<FileAccess> file = _open_persistence_fixture(p_path, FileAccess::READ_WRITE);
    if (file.is_null()) {
        return false;
    }
    const uint64_t file_length = file->get_length();
    file->seek(0);
    while (file->get_position() + uint64_t(sizeof(GaussianSplatting::ChunkHeader)) <= file_length) {
        const uint64_t chunk_start = file->get_position();
        const uint32_t chunk_type = file->get_32();
        const uint32_t chunk_size = file->get_32();
        file->get_32(); // checksum
        file->get_32(); // flags
        const uint64_t payload_offset = file->get_position();
        if (payload_offset > file_length || uint64_t(chunk_size) > file_length - payload_offset) {
            return false;
        }
        if (chunk_type == uint32_t(GaussianSplatting::ChunkType::END_OF_FILE)) {
            file->seek(chunk_start + sizeof(uint32_t)); // past the type field
            file->store_32(p_declared_size);
            return true;
        }
        file->seek(payload_offset + uint64_t(chunk_size));
    }
    return false;
}

// #700: appends bytes after a complete, valid file. The chunk loop stops at the
// terminator, so nothing here is parsed or counted -- which is exactly why it
// used to load as OK.
bool _append_fixture_tail(const String &p_path, const PackedByteArray &p_bytes) {
    Ref<FileAccess> file = _open_persistence_fixture(p_path, FileAccess::READ_WRITE);
    if (file.is_null()) {
        return false;
    }
    file->seek_end();
    file->store_buffer(p_bytes);
    return true;
}

// #700: flips one byte inside the FIRST chunk payload of the given type, leaving
// the chunk header (and therefore its recorded checksum) untouched.
bool _tamper_chunk_payload_byte(const String &p_path, uint32_t p_chunk_type) {
    Ref<FileAccess> file = _open_persistence_fixture(p_path, FileAccess::READ_WRITE);
    if (file.is_null()) {
        return false;
    }
    const uint64_t file_length = file->get_length();
    file->seek(0);
    while (file->get_position() + uint64_t(sizeof(GaussianSplatting::ChunkHeader)) <= file_length) {
        const uint32_t chunk_type = file->get_32();
        const uint32_t chunk_size = file->get_32();
        file->get_32(); // checksum
        file->get_32(); // flags
        const uint64_t payload_offset = file->get_position();
        if (payload_offset > file_length || uint64_t(chunk_size) > file_length - payload_offset) {
            return false;
        }
        if (chunk_type == p_chunk_type && chunk_size > 0) {
            file->seek(payload_offset);
            const uint8_t original = file->get_8();
            file->seek(payload_offset);
            file->store_8(uint8_t(original ^ 0xFFu));
            return true;
        }
        if (chunk_type == uint32_t(GaussianSplatting::ChunkType::END_OF_FILE)) {
            break;
        }
        file->seek(payload_offset + uint64_t(chunk_size));
    }
    return false;
}

// Writes a checksum-disabled HEAD chunk (16-byte chunk header + 60-byte payload)
// mirroring GaussianSceneSerializer::_pack_scene_header, so a test can hand-build
// a structurally valid chunked GSF prefix.
void _write_gsf_header_chunk(Ref<FileAccess> file, uint32_t total_chunks, uint32_t splat_count) {
    // HEAD chunk header. checksum field 0 (validation disabled on the reader).
    file->store_32((uint32_t)GaussianSplatting::ChunkType::HEADER);
    file->store_32(GaussianSplatting::SCENE_HEADER_PACKED_SIZE); // 60-byte payload
    file->store_32(0); // checksum
    file->store_32(0); // flags
    // HEAD payload (see _pack_scene_header for the exact byte layout).
    file->store_32(GaussianSplatting::GAUSSIAN_SCENE_MAGIC);
    file->store_16(GaussianSplatting::GAUSSIAN_SCENE_VERSION);
    file->store_16(0); // scene flags: SCENE_FLAG_CHECKSUM_ENABLED NOT set
    file->store_32(total_chunks);
    file->store_32(splat_count);
    for (int i = 0; i < 3; i++) {
        file->store_float(0.0f); // bounds_min
    }
    for (int i = 0; i < 3; i++) {
        file->store_float(0.0f); // bounds_max
    }
    file->store_64(0); // creation_time
    file->store_64(0); // modification_time
    file->store_16(GaussianSplatting::GAUSSIAN_SCENE_MIN_READER_VERSION);
    file->store_16(0); // _reserved_v2
}

Ref<GaussianSplatWorld> create_test_world() {
    Ref<GaussianData> data;
    data.instantiate();

    Vector<Gaussian> gaussians;
    gaussians.resize(3);
    for (int i = 0; i < 3; i++) {
        gaussians.write[i].position = Vector3(i, 0, 0);
        gaussians.write[i].scale = Vector3(1, 1, 1);
        gaussians.write[i].rotation = Quaternion();
        gaussians.write[i].opacity = 1.0f;
        gaussians.write[i].sh_dc = Color(1, 0, 0);
    }
    data->set_gaussians(gaussians);

    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(data);
    world->set_bounds(data->get_aabb());

    return world;
}

// Deterministic seeded GaussianData for the incremental-saver data-integrity
// cases below. Every field is a function of the index so a test can recompute the
// expected value (to prove a splat is unchanged, or that exactly one field moved).
// No incremental saver is attached here, so the internal set_gaussians() does not
// trip the PERSIST-001 structural-invalidation flag.
Ref<GaussianData> _make_seeded_gaussian_data(int p_count) {
    Ref<GaussianData> data;
    data.instantiate();
    Vector<Gaussian> gaussians;
    gaussians.resize(p_count);
    for (int i = 0; i < p_count; i++) {
        Gaussian &g = gaussians.write[i];
        g.position = Vector3(1.0f + i, 2.0f + 0.5f * i, 3.0f - 0.25f * i);
        g.scale = Vector3(1.0f + 0.1f * i, 1.0f, 1.0f);
        g.rotation = Quaternion();
        g.opacity = 0.10f + 0.05f * i;
        g.sh_dc = Color(0.2f + 0.05f * i, 0.3f, 0.4f, 1.0f);
    }
    data->set_gaussians(gaussians);
    return data;
}

} // namespace

TEST_CASE("[GaussianSplatting][Persistence] GSF round-trip serialization") {
    const String path = _make_persistence_fixture_path("test_roundtrip");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    // Dedicated LOCAL fixture (NOT the shared create_test_world() helper, which
    // other test cases depend on): give every splat DISTINCT, non-default values
    // for EVERY field the raw-record GAUSSIAN_DATA chunk persists. That chunk is a
    // whole-struct memcpy of each `Gaussian` (see _write_gaussian_data_chunk), so
    // position, opacity, scale, area, rotation, sh_dc (incl. alpha), normal,
    // stroke_age, brush_axes, painterly_meta and render_meta all round-trip. A
    // serializer/format migration that drops, zeroes, or defaults any of them
    // would fail this round-trip rather than silently pass on constant defaults.
    // (sh_1[] first-order SH is owned by the sibling SH case below; the _padding /
    // _padding2 lanes carry no semantics and are not asserted.)
    Ref<GaussianData> original_data;
    original_data.instantiate();

    Vector<Gaussian> gaussians;
    gaussians.resize(3);
    for (int i = 0; i < 3; i++) {
        Gaussian &g = gaussians.write[i];
        // Every component of every persisted field is seeded NON-ZERO and
        // distinct, so a regression that drops/zero-fills any single lane is
        // caught (a zero lane would match a zero expectation and hide the bug):
        // non-axis position, a non-axis-aligned rotation (all of x/y/z/w
        // non-zero), and an offset sh_dc so even splat 0 has non-zero rgb.
        g.position = Vector3(1.0f + i, 2.0f + 0.5f * i, 3.0f - 0.25f * i);
        g.scale = Vector3(1.0f + i, 2.0f + i, 3.0f + i);
        g.rotation = Quaternion(Vector3(1, 2, 3).normalized(), 0.3f * (i + 1)).normalized();
        g.opacity = 0.2f + 0.2f * i;
        g.sh_dc = Color(0.1f + 0.1f * i, 0.2f + 0.1f * i, 0.3f + 0.1f * i, 0.3f + 0.2f * i);

        // Remaining raw-record fields persisted by the whole-struct memcpy but
        // previously unguarded. sh_1[] is intentionally left at default here so
        // this case does not duplicate the sibling first-order SH test.
        g.area = 4.0f + 1.5f * i;
        g.normal = Vector3(0.2f + 0.1f * i, -0.5f - 0.1f * i, 0.8f + 0.1f * i);
        g.stroke_age = 9.0f + 3.0f * i;
        g.brush_axes = Vector2(0.5f + i, 1.25f + i);
        g.painterly_meta = gaussian_pack_painterly_meta(uint16_t(11 + i), uint16_t(101 + i));
        g.render_meta = uint32_t(0x00C0FFEEu + uint32_t(i));
    }
    original_data->set_gaussians(gaussians);
    CHECK_MESSAGE(original_data.is_valid(), "Original data should be valid");
    CHECK_MESSAGE(original_data->get_count() == 3, "Original should have 3 splats");

    GaussianSplatting::GaussianSceneSerializer serializer;
    Error save_err = serializer.save_scene(path, original_data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save should succeed");

    if (save_err != OK) return;

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();

    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == OK, "GSF load should succeed");
    CHECK_MESSAGE(loaded_data.is_valid(), "Loaded data should be valid");

    if (!loaded_data.is_valid()) return;

    CHECK_EQ(loaded_data->get_count(), 3);

    for (int i = 0; i < 3; i++) {
        Gaussian g = loaded_data->get_gaussian(i);

        // Position — all three lanes non-zero (matches the seed).
        CHECK(g.position.is_equal_approx(Vector3(1.0f + i, 2.0f + 0.5f * i, 3.0f - 0.25f * i)));

        // Scale / rotation: exact Vector3/Quaternion comparison via is_equal_approx.
        CHECK(g.scale.is_equal_approx(Vector3(1.0f + i, 2.0f + i, 3.0f + i)));
        const Quaternion expected_rot = Quaternion(Vector3(1, 2, 3).normalized(), 0.3f * (i + 1)).normalized();
        // Quaternion sign ambiguity (q and -q encode the same rotation): compare
        // COMPONENT-WISE against expected_rot or -expected_rot. The rotation is
        // about a non-axis vector, so x/y/z/w are all non-zero — no lane is hidden
        // by a zero expectation, and a dot-product's zero-component blind spot is
        // avoided entirely.
        CHECK((g.rotation.is_equal_approx(expected_rot) ||
                g.rotation.is_equal_approx(-expected_rot)));

        // Opacity travels as a float; small abs-diff tolerance.
        CHECK(Math::abs(g.opacity - (0.2f + 0.2f * i)) < 0.02f);

        // DC color (sh_dc): all four lanes are persisted raw — rgb plus the alpha
        // lane seeded above. First-order/high-order SH stay out of scope here
        // (the sibling SH test cases own first-order SH and high-order loss).
        const Color expected_dc = Color(0.1f + 0.1f * i, 0.2f + 0.1f * i, 0.3f + 0.1f * i, 0.3f + 0.2f * i);
        CHECK(Math::abs(g.sh_dc.r - expected_dc.r) < 0.02f);
        CHECK(Math::abs(g.sh_dc.g - expected_dc.g) < 0.02f);
        CHECK(Math::abs(g.sh_dc.b - expected_dc.b) < 0.02f);
        CHECK(Math::abs(g.sh_dc.a - expected_dc.a) < 0.02f);

        // Remaining raw-record fields. The whole-struct memcpy (plus lossless
        // chunk compression) is bit-exact, so these round-trip exactly; the
        // is_equal_approx / exact-int guards catch a format migration that would
        // drop or default any of them.
        CHECK(Math::is_equal_approx(g.area, 4.0f + 1.5f * i));
        CHECK(g.normal.is_equal_approx(Vector3(0.2f + 0.1f * i, -0.5f - 0.1f * i, 0.8f + 0.1f * i)));
        CHECK(Math::is_equal_approx(g.stroke_age, 9.0f + 3.0f * i));
        CHECK(g.brush_axes.is_equal_approx(Vector2(0.5f + i, 1.25f + i)));
        CHECK_EQ(g.painterly_meta, gaussian_pack_painterly_meta(uint16_t(11 + i), uint16_t(101 + i)));
        CHECK_EQ(g.render_meta, uint32_t(0x00C0FFEEu + uint32_t(i)));
    }

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] GSF round-trip preserves first-order SH metadata and payload") {
    // First-order SH lives inside the persisted `Gaussian` struct (sh_1[3]) and
    // must survive a round-trip. The reconstruction path must also rebuild
    // GaussianData::get_sh_first_order_count() so downstream consumers see a
    // non-zero SH metadata count.
    const String path = _make_persistence_fixture_path("test_sh_first_order_roundtrip");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianData> original_data;
    original_data.instantiate();

    Vector<Gaussian> gaussians;
    gaussians.resize(4);
    for (int i = 0; i < 4; i++) {
        Gaussian &g = gaussians.write[i];
        g.position = Vector3(i, 0, 0);
        g.scale = Vector3(1, 1, 1);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(0.5f, 0.5f, 0.5f, 1.0f);
        // Populate all three first-order SH bands with non-zero values so the
        // derived sh_first_order_count is exactly 3.
        g.sh_1[0] = Vector3(0.10f + 0.01f * i, 0.20f, 0.30f);
        g.sh_1[1] = Vector3(-0.15f, 0.25f + 0.01f * i, -0.05f);
        g.sh_1[2] = Vector3(0.08f, -0.12f, 0.40f + 0.01f * i);
    }
    original_data->set_gaussians(gaussians);
    CHECK_MESSAGE(original_data->get_sh_first_order_count() == 3,
            "Source data should advertise sh_first_order_count == 3 before save");

    GaussianSplatting::GaussianSceneSerializer serializer;
    Error save_err = serializer.save_scene(path, original_data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save should succeed");
    if (save_err != OK) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == OK, "GSF load should succeed");

    if (load_err == OK) {
        CHECK_MESSAGE(loaded_data->get_sh_first_order_count() == 3,
                "Reconstructed GaussianData must report sh_first_order_count == 3");
        CHECK_EQ(loaded_data->get_count(), 4);
        for (int i = 0; i < loaded_data->get_count(); i++) {
            Gaussian g = loaded_data->get_gaussian(i);
            CHECK(g.sh_1[0].is_equal_approx(Vector3(0.10f + 0.01f * i, 0.20f, 0.30f)));
            CHECK(g.sh_1[1].is_equal_approx(Vector3(-0.15f, 0.25f + 0.01f * i, -0.05f)));
            CHECK(g.sh_1[2].is_equal_approx(Vector3(0.08f, -0.12f, 0.40f + 0.01f * i)));
        }
    }

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] GSF save/load drops high-order SH and 2D-mode flag (KNOWN LIMITATION, issue #600)") {
    // KNOWN LIMITATION (issue #600), pinned here DELIBERATELY -- this is not the
    // desired end state. The GAUSSIAN_DATA chunk persists only the per-splat
    // `Gaussian` struct bytes (which embed the first-order SH triplet). It does
    // NOT persist the high-order SH sidecar (`sh_high_order_coefficients`) or the
    // 2D-mode flag, so BOTH reset to their defaults after a save/load round-trip.
    //
    // save_scene() emits a runtime WARNING when a save would drop either of these
    // (see _write_scene_to_file), so the loss is observable rather than silent.
    // A lossless versioned schema is deferred to the format ADR tracked by #600;
    // when it lands, this test AND the reconstruction path in
    // _read_gaussian_data_chunk must both change to round-trip the sidecar + flag.
    const String path = _make_persistence_fixture_path("test_sh_high_order_and_2d_loss");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianData> original_data;
    original_data.instantiate();

    LocalVector<Gaussian> gaussians;
    gaussians.resize(2);
    for (uint32_t i = 0; i < gaussians.size(); i++) {
        Gaussian &g = gaussians[i];
        g.position = Vector3(i, 0, 0);
        g.scale = Vector3(1, 1, 1);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(1, 1, 1, 1);
        g.sh_1[0] = Vector3(0.1f, 0.2f, 0.3f);
    }

    LocalVector<Vector3> high_order;
    const uint32_t high_order_per_splat = 5;
    high_order.resize(gaussians.size() * high_order_per_splat);
    for (uint32_t i = 0; i < high_order.size(); i++) {
        high_order[i] = Vector3(float(i) * 0.01f, 0.0f, 0.0f);
    }

    // Seed BOTH lossy dimensions: the high-order SH sidecar AND 2D (surfel) mode.
    original_data->set_gaussian_payload(gaussians, high_order, 1, high_order_per_splat, true);
    CHECK_MESSAGE(original_data->get_sh_high_order_count() == high_order_per_splat,
            "Source data should carry high-order SH before save");
    CHECK_MESSAGE(original_data->get_2d_mode(),
            "Source data should be flagged 2D before save");

    GaussianSplatting::GaussianSceneSerializer serializer;
    // NOTE: this save intentionally hits the lossy path, so it emits the #600
    // runtime warning about dropping the high-order SH sidecar + 2D-mode flag.
    Error save_err = serializer.save_scene(path, original_data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save should succeed");
    if (save_err != OK) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == OK, "GSF load should succeed");
    if (load_err == OK) {
        CHECK_MESSAGE(loaded_data->get_sh_high_order_count() == 0,
                "KNOWN LIMITATION #600: high-order SH is not persisted and must reset to 0 on load");
        CHECK_MESSAGE(loaded_data->get_sh_high_order_coefficients_ptr() == nullptr,
                "KNOWN LIMITATION #600: high-order SH sidecar must be empty after reconstruction");
        CHECK_MESSAGE(loaded_data->get_2d_mode() == false,
                "KNOWN LIMITATION #600: the 2D-mode flag is not persisted and must reset to false on load");
        // First-order SH still survives because it is embedded in the Gaussian struct bytes.
        CHECK_MESSAGE(loaded_data->get_sh_first_order_count() == 1,
                "First-order SH metadata should be recovered from the persisted Gaussian bytes");
    }

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] GSF load clears pre-existing state on the target GaussianData") {
    // Loading into a GaussianData that already carries derived/overlay state
    // must funnel through the canonical invalidation path so leftover high-order
    // SH / overlays / octree state do not survive into the loaded resource.
    const String path = _make_persistence_fixture_path("test_load_clears_state");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> source_data = world->get_gaussian_data();
    CHECK(source_data.is_valid());

    GaussianSplatting::GaussianSceneSerializer serializer;
    Error save_err = serializer.save_scene(path, source_data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save should succeed");
    if (save_err != OK) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();

    // Seed the target with leftover high-order SH + an overlay modification
    // that must be wiped on load.
    LocalVector<Gaussian> seed_gaussians;
    seed_gaussians.resize(5);
    LocalVector<Vector3> seed_high_order;
    seed_high_order.resize(seed_gaussians.size() * 3);
    loaded_data->set_gaussian_payload(seed_gaussians, seed_high_order, 0, 3, false);
    loaded_data->set_runtime_position(0, Vector3(42.0f, 0.0f, 0.0f));
    CHECK_MESSAGE(loaded_data->has_modifications(),
            "Seeded data should carry a runtime overlay modification before load");
    CHECK_MESSAGE(loaded_data->get_sh_high_order_count() == 3,
            "Seeded data should carry high-order SH before load");

    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == OK, "GSF load should succeed");
    if (load_err == OK) {
        CHECK_EQ(loaded_data->get_count(), source_data->get_count());
        CHECK_MESSAGE(!loaded_data->has_modifications(),
                "Runtime overlays must be cleared when load replaces storage");
        CHECK_MESSAGE(loaded_data->get_sh_high_order_count() == 0,
                "Stale high-order SH must be cleared when load replaces storage");
    }

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-001a structural setter forces save_changes to fail closed") {
    // A structural batch setter (set_gaussians) replaces the WHOLE splat array,
    // which the per-index change list cannot represent. save_changes() must then
    // fail CLOSED (ERR_UNAVAILABLE) instead of silently writing an empty/partial
    // delta and returning OK -- that silent OK is the PERSIST-001 data-loss bug.
    //
    // MUTATION that flips this case RED: delete EITHER
    //   (a) the `if (incremental_saver.is_valid()) incremental_saver->mark_requires_full_save();`
    //       added to GaussianData::set_gaussians(), OR
    //   (b) the `ERR_FAIL_COND_V_MSG(requires_full_save, ERR_UNAVAILABLE, ...)` guard
    //       at the top of GaussianIncrementalSaver::save_changes().
    // With either gone, save_changes() returns OK and the ERR_UNAVAILABLE CHECK fails.
    const String baseline_path = _make_persistence_fixture_path("persist001a_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist001a_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(4);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);

    // Establish a valid baseline and enable tracking (both clear requires_full_save).
    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    CHECK(saver->is_tracking_enabled());

    // Sanity: with a clean baseline and no structural edit, save_changes() succeeds.
    // This proves the ERR_UNAVAILABLE below is caused by the structural edit, not a
    // broken setup.
    CHECK_EQ(saver->save_changes(delta_path), OK);
    CHECK_MESSAGE(!saver->get_requires_full_save(), "flag must be clear before the structural edit");

    // STRUCTURAL replace of all splats through the public setter (different count
    // AND different contents).
    Vector<Gaussian> replacement;
    replacement.resize(6);
    for (int i = 0; i < replacement.size(); i++) {
        Gaussian &g = replacement.write[i];
        g.position = Vector3(-1.0f * i, 0.0f, 0.0f);
        g.scale = Vector3(1, 1, 1);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(0, 1, 0, 1);
    }
    data->set_gaussians(replacement);

    CHECK_MESSAGE(saver->get_requires_full_save(),
            "set_gaussians() must mark the saver as requiring a full re-baseline");

    const Error save_err = saver->save_changes(delta_path);
    CHECK_MESSAGE(save_err == ERR_UNAVAILABLE,
            "save_changes() must fail closed with ERR_UNAVAILABLE after a structural batch edit");
    CHECK_MESSAGE(save_err != OK,
            "save_changes() must NOT silently return OK -- that is the PERSIST-001 data-loss bug");

    // Recovery contract: a full re-baseline clears the flag and re-enables saving.
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    CHECK_MESSAGE(!saver->get_requires_full_save(), "create_baseline() must clear the flag");
    CHECK_EQ(saver->save_changes(delta_path), OK);

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-001b set_gaussian records a per-index delta that round-trips") {
    // The single-index setter set_gaussian() must record the edit into the
    // incremental saver so a save/load round-trip reproduces it. Before the fix
    // set_gaussian() mutated storage WITHOUT recording, so save_changes() wrote an
    // empty delta and the edit was silently lost on reload (PERSIST-001).
    //
    // MUTATION that flips this case RED: delete the
    //   `if (incremental_saver.is_valid() && !incremental_saver->is_applying())
    //        incremental_saver->record_splat_change(...)`
    // added to GaussianData::set_gaussian(). Then get_splat_change_count() stays 0,
    // the saved delta is empty, and the reloaded copy still holds the ORIGINAL
    // opacity -- both the change-count CHECK and the round-trip CHECK fail.
    const int N = 5;
    const int edit_index = 2;
    const float edited_opacity = 0.875f;

    const String baseline_path = _make_persistence_fixture_path("persist001b_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist001b_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);

    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);

    // Edit exactly one splat's opacity through the single-index setter.
    Gaussian g = data->get_gaussian(edit_index);
    const float original_opacity = g.opacity;
    CHECK_MESSAGE(Math::abs(original_opacity - edited_opacity) > 0.01f,
            "sanity: the edited opacity must differ from the seeded value");
    g.opacity = edited_opacity;
    data->set_gaussian(edit_index, g);

    CHECK_MESSAGE(saver->get_splat_change_count() == 1,
            "set_gaussian() must record exactly one per-index splat change");

    CHECK_EQ(saver->save_changes(delta_path), OK);

    // Apply the delta onto a FRESH copy of the baseline (identical seeded content,
    // same count N) and confirm the edit reproduces.
    Ref<GaussianData> fresh = _make_seeded_gaussian_data(N);
    CHECK_MESSAGE(Math::abs(fresh->get_gaussian(edit_index).opacity - original_opacity) < 0.001f,
            "sanity: the fresh copy starts at the original opacity, pre-apply");

    const Error apply_err = saver->load_and_apply_changes(delta_path, fresh.ptr());
    CHECK_EQ(apply_err, OK);
    CHECK_MESSAGE(Math::abs(fresh->get_gaussian(edit_index).opacity - edited_opacity) < 0.001f,
            "the set_gaussian() edit must round-trip through save/load into the fresh copy");
    // A single-index delta must not perturb neighbouring splats.
    if (edit_index + 1 < N) {
        const float neighbour_expected = 0.10f + 0.05f * float(edit_index + 1);
        CHECK_MESSAGE(Math::abs(fresh->get_gaussian(edit_index + 1).opacity - neighbour_expected) < 0.001f,
                "a single-index delta must not perturb other splats");
    }

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-002 delta refuses to apply against a mismatched baseline") {
    // A delta recorded against a baseline of N splats must NOT be applied to a
    // GaussianData with a different splat count. Doing so would apply the in-range
    // per-index edits and silently drop the rest (cross-baseline corruption). The
    // apply must fail with ERR_INVALID_DATA and leave the target COMPLETELY
    // unchanged (PERSIST-002).
    //
    // The recorded edit is at index 0, which is IN RANGE for BOTH counts, so this
    // case pins the baseline-COUNT check specifically (not the out-of-range guard).
    //
    // MUTATION that flips this case RED: neutralize the baseline count check
    //   `ERR_FAIL_COND_V_MSG((uint32_t)gaussian_data->get_count() != loaded_baseline_splat_count, ...)`
    // in load_and_apply_changes(). Then the index-0 edit applies to the M-splat
    // target, load returns OK, and the target's splat 0 is mutated -- both the
    // ERR_INVALID_DATA CHECK and the "target unchanged" CHECK fail.
    const int N = 5; // baseline the delta is recorded against
    const int M = 3; // mismatched target (M < N; index 0 is valid in both)
    const int edit_index = 0;
    const float edited_opacity = 0.9f;

    const String baseline_path = _make_persistence_fixture_path("persist002_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist002_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    // Record a delta against an N-splat baseline.
    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);

    Gaussian g = data->get_gaussian(edit_index);
    g.opacity = edited_opacity;
    data->set_gaussian(edit_index, g);
    CHECK_MESSAGE(saver->get_splat_change_count() == 1, "sanity: the edit was recorded");
    CHECK_EQ(saver->save_changes(delta_path), OK);

    // Attempt to apply that N-baseline delta onto an M-splat target (M != N).
    Ref<GaussianData> mismatched = _make_seeded_gaussian_data(M);
    const float target_original_opacity = mismatched->get_gaussian(edit_index).opacity;
    CHECK_MESSAGE(Math::abs(target_original_opacity - edited_opacity) > 0.01f,
            "sanity: the target's original opacity differs from the delta's edit");

    const Error apply_err = saver->load_and_apply_changes(delta_path, mismatched.ptr());
    CHECK_MESSAGE(apply_err == ERR_INVALID_DATA,
            "applying a delta to a differently-sized baseline must fail with ERR_INVALID_DATA");

    // The target must be COMPLETELY unchanged (nothing applied).
    CHECK_EQ(mismatched->get_count(), M);
    CHECK_MESSAGE(Math::abs(mismatched->get_gaussian(edit_index).opacity - target_original_opacity) < 0.0001f,
            "a rejected cross-baseline apply must leave the target splat unchanged");

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-001c create_baseline discards stale pre-baseline deltas") {
    // Recovery flow: after recording a per-index edit, establishing a NEW baseline
    // serializes the CURRENT splats in full -- so the earlier per-index delta is
    // already captured and MUST be discarded. If create_baseline() leaves the stale
    // change list intact, the next save_changes() emits a delta carrying a
    // pre-baseline edit that, replayed against the new baseline, re-mutates a splat
    // the user never touched after the baseline (silent corruption -- Codex PERSIST P1).
    //
    // MUTATION that flips this case RED: delete the `clear_changes();` added at the
    // end of GaussianIncrementalSaver::create_baseline(). Then get_splat_change_count()
    // stays 1 after the second baseline, and the stale edit reappears on apply.
    const int N = 5;
    const int stale_index = 2;
    const float stale_opacity = 0.123f;

    const String baseline_path = _make_persistence_fixture_path("persist001c_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist001c_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);

    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);

    // Record a per-index edit against the FIRST baseline.
    Gaussian g = data->get_gaussian(stale_index);
    const float seeded_opacity = g.opacity;
    CHECK_MESSAGE(Math::abs(seeded_opacity - stale_opacity) > 0.01f,
            "sanity: the stale edit differs from the seeded value");
    g.opacity = stale_opacity;
    data->set_gaussian(stale_index, g);
    CHECK_MESSAGE(saver->get_splat_change_count() == 1, "sanity: the pre-baseline edit was recorded");

    // Establish a NEW baseline capturing the current (already-edited) state in full.
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    CHECK_MESSAGE(saver->get_splat_change_count() == 0,
            "create_baseline() must discard deltas recorded before it -- they are already captured in the full baseline");

    // End-to-end: the delta emitted after the new baseline must be a NO-OP. Apply it
    // onto a FRESH seeded copy (index `stale_index` still at its seeded value) and
    // confirm the stale edit does NOT reappear.
    CHECK_EQ(saver->save_changes(delta_path), OK);
    Ref<GaussianData> fresh = _make_seeded_gaussian_data(N);
    const Error apply_err = saver->load_and_apply_changes(delta_path, fresh.ptr());
    CHECK_EQ(apply_err, OK);
    CHECK_MESSAGE(Math::abs(fresh->get_gaussian(stale_index).opacity - seeded_opacity) < 0.001f,
            "a discarded pre-baseline delta must not resurrect the stale edit on apply");

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-002b an unknown-baseline (count==0) splat delta is refused, target untouched") {
    // A delta saved WITHOUT a baseline (start_tracking on a path that does not exist yet)
    // writes baseline_splat_count == 0. Such a delta carries no verifiable baseline
    // identity, so even with EVERY index in range it must NOT be applied to a target: it
    // could be a delta from an unrelated scene and would corrupt the matching indices
    // (Codex F4). The apply must fail ERR_INVALID_DATA and leave the target untouched.
    //
    // MUTATION that flips this case RED: remove the count==0 rejection (F4) in
    // load_and_apply_changes(). The in-range indices then pass the (skipped) count check
    // and the index check, so the edits apply and mutate the target.
    const int N = 3;
    const float edit_opacity = 0.815f;

    const String baseline_path = _make_persistence_fixture_path("persist002b_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist002b_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    // Record in-range edits (indices 0..N-1) WITHOUT a baseline, so the saved delta
    // carries baseline_splat_count == 0.
    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path); // baseline_path does not exist yet -> count stays 0

    for (int i = 0; i < N; i++) {
        Gaussian g = data->get_gaussian(i);
        g.opacity = edit_opacity;
        data->set_gaussian(i, g);
    }
    CHECK_MESSAGE(saver->get_splat_change_count() == N,
            "sanity: in-range edits recorded on the unknown-baseline source");
    CHECK_EQ(saver->save_changes(delta_path), OK);

    // Apply onto a fresh N-splat target. Every index is IN range, so only the count==0
    // identity refusal can reject it; the whole apply must fail and leave the target at
    // its original seeded values.
    Ref<GaussianData> target = _make_seeded_gaussian_data(N);
    const float orig0 = target->get_gaussian(0).opacity;
    const float orig1 = target->get_gaussian(1).opacity;
    CHECK_MESSAGE(Math::abs(orig0 - edit_opacity) > 0.01f,
            "sanity: target splat 0 starts different from the delta's edit");
    CHECK_MESSAGE(Math::abs(orig1 - edit_opacity) > 0.01f,
            "sanity: target splat 1 starts different from the delta's edit");

    const Error apply_err = saver->load_and_apply_changes(delta_path, target.ptr());
    CHECK_MESSAGE(apply_err == ERR_INVALID_DATA,
            "an unknown-baseline (count==0) splat delta must be refused with ERR_INVALID_DATA");
    CHECK_MESSAGE(Math::abs(target->get_gaussian(0).opacity - orig0) < 0.0001f,
            "a refused count==0 apply must leave target splat 0 unchanged");
    CHECK_MESSAGE(Math::abs(target->get_gaussian(1).opacity - orig1) < 0.0001f,
            "a refused count==0 apply must leave target splat 1 unchanged");

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-001d bulk mutators fail save_changes closed") {
    // Bulk/structural mutators the per-index delta cannot represent -- a field overwrite
    // (set_positions, via _invalidate_derived_caches_locked), a structural resize (via
    // _on_gaussian_storage_changed_locked), and a prune (explicit hook) -- must all fail
    // save_changes() closed (ERR_UNAVAILABLE) through the shared
    // _invalidate_incremental_delta_locked() hook, never silently save an empty/partial
    // delta (PERSIST-001).
    //
    // MUTATION that flips a leg RED: remove the _invalidate_incremental_delta_locked()
    // call from the matching GaussianData hook (_invalidate_derived_caches_locked /
    // _on_gaussian_storage_changed_locked / prune_by_importance). That mutator then leaves
    // requires_full_save false and save_changes() returns OK.
    const int N = 6;
    const String baseline_path = _make_persistence_fixture_path("persist001d_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist001d_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path);

    // Leg 1: per-property bulk field overwrite (same count).
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    CHECK_EQ(saver->save_changes(delta_path), OK); // a clean baseline still saves fine
    PackedVector3Array positions;
    positions.resize(N);
    for (int i = 0; i < N; i++) {
        positions.set(i, Vector3(-1.0f * i, 2.0f, 3.0f));
    }
    data->set_positions(positions);
    CHECK_MESSAGE(saver->get_requires_full_save(), "set_positions() must invalidate the delta");
    CHECK_MESSAGE(saver->save_changes(delta_path) == ERR_UNAVAILABLE, "save must fail closed after set_positions()");

    // Leg 2: structural resize (count change).
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    CHECK_MESSAGE(!saver->get_requires_full_save(), "re-baseline clears the flag");
    data->resize(N + 3);
    CHECK_MESSAGE(saver->get_requires_full_save(), "resize() must invalidate the delta");
    CHECK_MESSAGE(saver->save_changes(delta_path) == ERR_UNAVAILABLE, "save must fail closed after resize()");

    // Leg 3: prune (drops splats).
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    CHECK_MESSAGE(!saver->get_requires_full_save(), "re-baseline clears the flag");
    const uint32_t before = (uint32_t)data->get_count();
    const uint32_t kept = data->prune_by_importance(0.5, 0.0f);
    CHECK_MESSAGE(kept < before, "sanity: prune actually dropped splats");
    CHECK_MESSAGE(saver->get_requires_full_save(), "prune_by_importance() must invalidate the delta");
    CHECK_MESSAGE(saver->save_changes(delta_path) == ERR_UNAVAILABLE, "save must fail closed after prune");

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-001e set_gaussian on an unsupported field fails save closed") {
    // The per-index delta serializes only position/sh_dc/opacity/scale/rotation. A
    // set_gaussian() that changes ONLY an unsupported field (here painterly_meta) must NOT
    // record a lossy partial/empty delta -- it must fail save_changes() closed so the field
    // is preserved via a full re-baseline (PERSIST-001).
    //
    // MUTATION that flips this case RED: delete the _gaussian_unsupported_field_changed()
    // early-out in GaussianIncrementalSaver::_track_splat_change(). The change then records
    // nothing (mask==0 -> return), get_requires_full_save() stays false, and save returns OK.
    const int N = 5;
    const int edit_index = 2;
    const String baseline_path = _make_persistence_fixture_path("persist001e_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist001e_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);

    // Change ONLY painterly_meta (an unsupported field); every supported field stays equal.
    Gaussian g = data->get_gaussian(edit_index);
    CHECK_MESSAGE(g.painterly_meta == 0u, "sanity: seeded painterly_meta is 0");
    g.painterly_meta = 0xABCDu;
    data->set_gaussian(edit_index, g);

    CHECK_MESSAGE(saver->get_splat_change_count() == 0,
            "an unsupported-field-only change must NOT record a per-index delta");
    CHECK_MESSAGE(saver->get_requires_full_save(),
            "an unsupported-field change must fail the delta closed");
    CHECK_MESSAGE(saver->save_changes(delta_path) == ERR_UNAVAILABLE,
            "save_changes() must fail closed rather than drop the unsupported-field edit");

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-002d update_baseline refreshes the baseline splat count") {
    // After a structural rebase through update_baseline() onto a baseline with a DIFFERENT
    // splat count, deltas recorded against the new baseline must round-trip. If
    // update_baseline() fails to refresh baseline_splat_count from the new baseline file,
    // save_changes() stamps the stale count into the delta header and the load-time count
    // check rejects it against the correctly-sized target (PERSIST-002 / Codex).
    //
    // MUTATION that flips this case RED: remove the get_file_info()/baseline_splat_count
    // refresh added to update_baseline(). The delta then carries the OLD (3) count and the
    // apply onto an 8-splat target fails ERR_INVALID_DATA instead of OK.
    const int SMALL = 3;
    const int BIG = 8;
    const int edit_index = 0;
    const float edited_opacity = 0.77f;

    const String small_baseline = _make_persistence_fixture_path("persist002d_small", ".gsf");
    const String big_baseline = _make_persistence_fixture_path("persist002d_big", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist002d_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(small_baseline) && _ensure_persistence_fixture_dir(big_baseline) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    // Write a SMALL (3-splat) and a BIG (8-splat) baseline file on disk.
    {
        Ref<GaussianData> small_data = _make_seeded_gaussian_data(SMALL);
        Ref<GaussianData> big_data = _make_seeded_gaussian_data(BIG);
        Ref<GaussianSplatting::GaussianIncrementalSaver> writer;
        writer.instantiate();
        writer->start_tracking(small_baseline);
        CHECK_EQ(writer->create_baseline(small_baseline, small_data.ptr()), OK);
        CHECK_EQ(writer->create_baseline(big_baseline, big_data.ptr()), OK);
    }

    // A saver that starts tracking the SMALL baseline (count = 3) ...
    Ref<GaussianData> data = _make_seeded_gaussian_data(BIG);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(small_baseline); // reads count = 3 from the small baseline

    // ... then rebases onto the BIG baseline (count = 8) via update_baseline().
    CHECK_EQ(saver->update_baseline(big_baseline), OK);

    // Record a per-index edit against the 8-splat data and save the delta.
    Gaussian g = data->get_gaussian(edit_index);
    g.opacity = edited_opacity;
    data->set_gaussian(edit_index, g);
    CHECK_MESSAGE(saver->get_splat_change_count() == 1, "sanity: the edit was recorded");
    CHECK_EQ(saver->save_changes(delta_path), OK);

    // Apply onto a fresh 8-splat target: the count check must pass (8 == refreshed 8).
    Ref<GaussianData> fresh = _make_seeded_gaussian_data(BIG);
    const Error apply_err = saver->load_and_apply_changes(delta_path, fresh.ptr());
    CHECK_MESSAGE(apply_err == OK,
            "update_baseline() must refresh the count so an 8-splat delta applies to an 8-splat target");
    CHECK_MESSAGE(Math::abs(fresh->get_gaussian(edit_index).opacity - edited_opacity) < 0.001f,
            "the edit recorded after the rebase must round-trip");

    _remove_persistence_fixture(small_baseline);
    _remove_persistence_fixture(big_baseline);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-002c a refused count==0 load preserves the saver's pending edits") {
    // load_and_apply_changes() clears and replaces the saver's pending change tables when
    // it commits a loaded delta. An unverifiable count==0 delta must be rejected BEFORE
    // that commit (F4), so a tracking saver that still holds unsaved edits does not lose
    // them to the loaded file's entries (Codex: validate before committing saver state).
    //
    // MUTATION that flips this case RED: remove the count==0 rejection (F4) in
    // load_and_apply_changes(). The in-range delta then reaches the commit, which
    // clears+rebuilds splat_changes from the file (N entries), so the saver's pending
    // edit count becomes N instead of the preserved 1.
    const int N = 3;
    const String src_baseline = _make_persistence_fixture_path("persist002c_srcbase", ".gsf");
    const String loose_delta = _make_persistence_fixture_path("persist002c_loose", ".gsif");
    const String pending_baseline = _make_persistence_fixture_path("persist002c_pendbase", ".gsf");
    const bool dir_ready = _ensure_persistence_fixture_dir(src_baseline) && _ensure_persistence_fixture_dir(loose_delta) && _ensure_persistence_fixture_dir(pending_baseline);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    // Build an unknown-baseline (count==0) delta with IN-RANGE edits (0..N-1), so only the
    // count==0 identity refusal -- not the index check -- can reject it against the
    // N-splat target below.
    {
        Ref<GaussianData> src = _make_seeded_gaussian_data(N);
        Ref<GaussianSplatting::GaussianIncrementalSaver> src_saver;
        src_saver.instantiate();
        src->set_incremental_saver(src_saver);
        src_saver->start_tracking(src_baseline); // does not exist -> count stays 0
        for (int i = 0; i < N; i++) {
            Gaussian g = src->get_gaussian(i);
            g.opacity = 0.9f;
            src->set_gaussian(i, g);
        }
        CHECK_MESSAGE(src_saver->get_splat_change_count() == N, "sanity: in-range edits recorded on the source");
        CHECK_EQ(src_saver->save_changes(loose_delta), OK);
    }

    // A DIFFERENT, tracking saver with ONE unsaved pending edit on its own N-splat data.
    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(pending_baseline); // does not exist -> count 0
    Gaussian pe = data->get_gaussian(0);
    pe.opacity = 0.42f;
    data->set_gaussian(0, pe);
    CHECK_MESSAGE(saver->get_splat_change_count() == 1, "sanity: one pending edit is staged");

    // Loading the count==0 delta must fail WITHOUT discarding the pending edit.
    const Error apply_err = saver->load_and_apply_changes(loose_delta, data.ptr());
    CHECK_MESSAGE(apply_err == ERR_INVALID_DATA,
            "an unknown-baseline (count==0) delta must be refused with ERR_INVALID_DATA");
    CHECK_MESSAGE(saver->get_splat_change_count() == 1,
            "the saver's pending edit must survive a refused load (no state committed)");

    _remove_persistence_fixture(src_baseline);
    _remove_persistence_fixture(loose_delta);
    _remove_persistence_fixture(pending_baseline);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-001f set_spherical_harmonics fails save_changes closed") {
    // SH coefficients (high-order + first-order layout) are outside the per-index delta
    // contract, which serializes only sh_dc. Both set_spherical_harmonics() overloads
    // must fail save_changes() closed (ERR_UNAVAILABLE) rather than silently drop the SH
    // edit on reload (PERSIST-001 / Codex F1).
    //
    // MUTATION that flips a leg RED: remove the _invalidate_incremental_delta_locked()
    // call from the corresponding set_spherical_harmonics() overload. That overload then
    // leaves requires_full_save false and save_changes() returns OK.
    const int N = 4;
    const String baseline_path = _make_persistence_fixture_path("persist001f_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist001f_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path);

    // Leg 1: bulk overload (one RGB DC triplet per gaussian).
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    PackedFloat32Array sh;
    sh.resize(N * 3);
    for (int i = 0; i < sh.size(); i++) {
        sh.set(i, 0.5f);
    }
    data->set_spherical_harmonics(sh);
    CHECK_MESSAGE(saver->get_requires_full_save(), "bulk set_spherical_harmonics() must invalidate the delta");
    CHECK_MESSAGE(saver->save_changes(delta_path) == ERR_UNAVAILABLE, "save must fail closed after a bulk SH edit");

    // Leg 2: per-index overload.
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    CHECK_MESSAGE(!saver->get_requires_full_save(), "re-baseline clears the flag");
    const float coeffs[3] = { 0.1f, 0.2f, 0.3f };
    data->set_spherical_harmonics(1, coeffs, 3);
    CHECK_MESSAGE(saver->get_requires_full_save(), "per-index set_spherical_harmonics() must invalidate the delta");
    CHECK_MESSAGE(saver->save_changes(delta_path) == ERR_UNAVAILABLE, "save must fail closed after a per-index SH edit");

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-002f update_baseline rejects an invalid baseline and retains fail-closed state") {
    // update_baseline() must validate the new baseline BEFORE clearing pending changes /
    // the requires_full_save guard. Rebasing onto a missing or corrupt baseline must
    // return an error and leave the fail-closed state intact, so a prior structural edit
    // is not silently downgraded to a lossy incremental save (PERSIST-001 / Codex F2).
    //
    // MUTATION that flips this case RED: remove the get_file_info() validation guard at
    // the top of update_baseline(). It then clears requires_full_save even for a missing
    // path, the update returns OK, and save_changes() no longer fails closed.
    const int N = 4;
    const String baseline_path = _make_persistence_fixture_path("persist002f_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist002f_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);

    // A structural edit puts the saver into the fail-closed state.
    Vector<Gaussian> replacement;
    replacement.resize(N + 2);
    for (int i = 0; i < replacement.size(); i++) {
        Gaussian &g = replacement.write[i];
        g.opacity = 1.0f;
        g.scale = Vector3(1, 1, 1);
        g.rotation = Quaternion();
    }
    data->set_gaussians(replacement);
    CHECK_MESSAGE(saver->get_requires_full_save(), "sanity: the structural edit set the fail-closed flag");

    // Rebasing onto a MISSING baseline must fail and NOT clear the flag.
    const String missing_baseline = _make_persistence_fixture_path("persist002f_missing", ".gsf");
    _remove_persistence_fixture(missing_baseline); // ensure it does not exist
    const Error rebase_err = saver->update_baseline(missing_baseline);
    CHECK_MESSAGE(rebase_err != OK,
            "update_baseline() onto a missing baseline must return an error");
    CHECK_MESSAGE(saver->get_requires_full_save(),
            "a rejected update_baseline() must retain the fail-closed flag");
    CHECK_MESSAGE(saver->save_changes(delta_path) == ERR_UNAVAILABLE,
            "save must still fail closed after a rejected rebase");

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-002g load rebuilds the dedup map so later edits index correctly") {
    // After load_and_apply_changes() commits a loaded delta, the dedup map
    // (splat_index_to_change) must be rebuilt to match the loaded splat_changes. If it
    // keeps stale entries, a subsequent set_gaussian() on a DIFFERENT index follows a
    // stale map slot and overwrites the wrong recorded change (Codex F5).
    //
    // MUTATION that flips this case RED: remove the splat_index_to_change rebuild
    // (clear + insert) from the load commit in load_and_apply_changes(). The stale map
    // then routes the post-load edit for index 1 onto the loaded index-0 entry, so the
    // change count stays 1 instead of growing to 2 (and the index-1 edit is lost).
    const int N = 4;
    const String base_path = _make_persistence_fixture_path("persist002g_base", ".gsf");
    const String src_base = _make_persistence_fixture_path("persist002g_srcbase", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist002g_delta", ".gsif");
    const String out_delta = _make_persistence_fixture_path("persist002g_out", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(base_path) && _ensure_persistence_fixture_dir(src_base) && _ensure_persistence_fixture_dir(delta_path) && _ensure_persistence_fixture_dir(out_delta);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    // A count>0 delta that edits index 0 (so it is accepted and committed on load).
    {
        Ref<GaussianData> src = _make_seeded_gaussian_data(N);
        Ref<GaussianSplatting::GaussianIncrementalSaver> src_saver;
        src_saver.instantiate();
        src->set_incremental_saver(src_saver);
        src_saver->start_tracking(src_base);
        CHECK_EQ(src_saver->create_baseline(src_base, src.ptr()), OK);
        Gaussian g0 = src->get_gaussian(0);
        g0.opacity = 0.33f;
        src->set_gaussian(0, g0);
        CHECK_EQ(src_saver->save_changes(delta_path), OK);
    }

    // The saver stages a pending edit at index 1 (map: 1 -> 0), then loads the index-0
    // delta (commit clears splat_changes + must rebuild the map to {0 -> 0}).
    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(base_path);
    CHECK_EQ(saver->create_baseline(base_path, data.ptr()), OK);

    Gaussian g1 = data->get_gaussian(1);
    g1.opacity = 0.71f;
    data->set_gaussian(1, g1); // pending: map[1] = 0
    CHECK_MESSAGE(saver->get_splat_change_count() == 1, "sanity: one pending edit at index 1");

    CHECK_EQ(saver->load_and_apply_changes(delta_path, data.ptr()), OK);
    CHECK_MESSAGE(saver->get_splat_change_count() == 1, "loaded delta committed exactly its one index-0 entry");

    // A NEW edit at index 1 must create a SECOND change (index 1 is not in the loaded
    // map), not fold into the loaded index-0 entry via a stale map slot.
    Gaussian g1b = data->get_gaussian(1);
    g1b.opacity = 0.62f;
    data->set_gaussian(1, g1b);
    CHECK_MESSAGE(saver->get_splat_change_count() == 2,
            "a post-load edit at a new index must add a distinct change (dedup map rebuilt)");

    // Round-trip: both the loaded index-0 edit and the post-load index-1 edit must apply.
    CHECK_EQ(saver->save_changes(out_delta), OK);
    Ref<GaussianData> fresh = _make_seeded_gaussian_data(N);
    CHECK_EQ(saver->load_and_apply_changes(out_delta, fresh.ptr()), OK);
    CHECK_MESSAGE(Math::abs(fresh->get_gaussian(0).opacity - 0.33f) < 0.001f,
            "the loaded index-0 edit round-trips");
    CHECK_MESSAGE(Math::abs(fresh->get_gaussian(1).opacity - 0.62f) < 0.001f,
            "the post-load index-1 edit round-trips to the correct splat");

    _remove_persistence_fixture(base_path);
    _remove_persistence_fixture(src_base);
    _remove_persistence_fixture(delta_path);
    _remove_persistence_fixture(out_delta);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-001g create_baseline retains animation edits it did not capture") {
    // create_baseline(path, data) with the default (no animation object) writes NO
    // animation chunk, so pending animation / clip-metadata edits are not captured by the
    // full save. Clearing them anyway (as an unconditional clear_changes() would) loses
    // the animation edit -- the baseline omits it AND the next delta omits it. Only the
    // splat deltas the baseline actually captured may be discarded (Codex).
    //
    // MUTATION that flips this case RED: replace the selective clear in create_baseline()
    // with an unconditional clear_changes(). The pending animation change is then dropped
    // and get_animation_change_count() becomes 0.
    const int N = 4;
    const String baseline_path = _make_persistence_fixture_path("persist001g_baseline", ".gsf");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);

    // Stage a pending splat edit AND a pending animation edit.
    Gaussian g = data->get_gaussian(0);
    g.opacity = 0.5f;
    data->set_gaussian(0, g);
    Dictionary before;
    Dictionary after;
    after["value"] = 1;
    saver->record_animation_change(0, (GaussianSplatting::AnimationProperty)0, before, after);
    CHECK_MESSAGE(saver->get_splat_change_count() == 1, "sanity: one pending splat edit");
    CHECK_MESSAGE(saver->get_animation_change_count() == 1, "sanity: one pending animation edit");

    // Re-baseline WITHOUT an animation object: the splat edit is captured (data), the
    // animation edit is NOT. The splat delta must be discarded, the animation edit kept.
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    CHECK_MESSAGE(saver->get_splat_change_count() == 0,
            "create_baseline captured the splat state, so its per-index delta is discarded");
    CHECK_MESSAGE(saver->get_animation_change_count() == 1,
            "create_baseline did NOT capture animation, so the pending animation edit is retained");

    _remove_persistence_fixture(baseline_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-002h start_tracking on an invalid baseline retains the fail-closed guard") {
    // start_tracking() must not clear the requires_full_save guard when the given baseline
    // is missing/corrupt. If a structural mutation set the guard and the caller then
    // re-tracks against an invalid baseline, clearing it would let save_changes() write a
    // lossy delta with no valid full baseline behind it (Codex; mirrors update_baseline).
    //
    // MUTATION that flips this case RED: clear requires_full_save unconditionally in
    // start_tracking() (drop the baseline-validity gate). The guard is then cleared and
    // save_changes() no longer fails closed.
    const int N = 4;
    const String baseline_path = _make_persistence_fixture_path("persist002h_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist002h_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);

    // A structural edit sets the fail-closed guard.
    Vector<Gaussian> replacement;
    replacement.resize(N + 1);
    for (int i = 0; i < replacement.size(); i++) {
        Gaussian &g = replacement.write[i];
        g.opacity = 1.0f;
        g.scale = Vector3(1, 1, 1);
        g.rotation = Quaternion();
    }
    data->set_gaussians(replacement);
    CHECK_MESSAGE(saver->get_requires_full_save(), "sanity: the structural edit set the fail-closed flag");

    // Re-tracking against a MISSING baseline must NOT clear the guard.
    const String missing_baseline = _make_persistence_fixture_path("persist002h_missing", ".gsf");
    _remove_persistence_fixture(missing_baseline); // ensure it does not exist
    saver->start_tracking(missing_baseline);
    CHECK_MESSAGE(saver->get_requires_full_save(),
            "start_tracking() on an invalid baseline must retain the fail-closed guard");
    CHECK_MESSAGE(saver->save_changes(delta_path) == ERR_UNAVAILABLE,
            "save must still fail closed after re-tracking against a missing baseline");

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

// File-local worker for PERSIST-002i: hammers per-index (opacity-only) edits on a shared
// GaussianData, routing each through the saver's record path (data_rwlock -> change_mutex).
struct PersistStressCtx {
    GaussianData *data = nullptr;
    int lo = 0;
    int hi = 0;
    int iters = 0;
    int edits_done = 0;
};
static void _persist_stress_worker(void *p_userdata) {
    PersistStressCtx *ctx = static_cast<PersistStressCtx *>(p_userdata);
    for (int it = 0; it < ctx->iters; it++) {
        for (int i = ctx->lo; i < ctx->hi; i++) {
            Gaussian g = ctx->data->get_gaussian(i);
            g.opacity = 0.05f + 0.001f * float((it * 7 + i) % 90);
            ctx->data->set_gaussian(i, g);
            ctx->edits_done++;
        }
    }
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-002i concurrent records and saves stay consistent (no deadlock)") {
    // Codex: the saver must participate in synchronization. Two worker threads record
    // per-index edits on a shared GaussianData (each routing through change_mutex under
    // data_rwlock) while the main thread repeatedly saves and reads stats. This must not
    // deadlock, crash, or corrupt saver state; change_mutex serialises save-vs-record so
    // a save never snapshots a half-written change table, and the per-index (opacity)
    // edits -- being representable -- never trip the fail-closed guard.
    const int N = 64;
    const int ITERS = 40;
    const String baseline_path = _make_persistence_fixture_path("persist002i_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist002i_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);

    PersistStressCtx ctx_a;
    ctx_a.data = data.ptr();
    ctx_a.lo = 0;
    ctx_a.hi = N / 2;
    ctx_a.iters = ITERS;
    PersistStressCtx ctx_b;
    ctx_b.data = data.ptr();
    ctx_b.lo = N / 2;
    ctx_b.hi = N;
    ctx_b.iters = ITERS;

    Thread worker_a;
    Thread worker_b;
    worker_a.start(_persist_stress_worker, &ctx_a);
    worker_b.start(_persist_stress_worker, &ctx_b);

    // Main thread contends on change_mutex via repeated save + stats reads while the
    // workers record. A fail-closed (ERR_UNAVAILABLE) here would mean a structural edit
    // leaked in -- there is none, so it must never happen.
    int save_failed_closed = 0;
    int save_other_error = 0;
    for (int s = 0; s < 80; s++) {
        Error e = saver->save_changes(delta_path);
        if (e == ERR_UNAVAILABLE) {
            save_failed_closed++;
        } else if (e != OK) {
            save_other_error++;
        }
        (void)saver->get_change_count();
    }

    worker_a.wait_to_finish();
    worker_b.wait_to_finish();

    // Reaching here proves no deadlock; the workers completed every edit.
    CHECK_MESSAGE(ctx_a.edits_done == (N / 2) * ITERS, "worker A completed all its edits");
    CHECK_MESSAGE(ctx_b.edits_done == (N - N / 2) * ITERS, "worker B completed all its edits");
    CHECK_MESSAGE(save_failed_closed == 0, "per-index edits must never trip the fail-closed guard");
    CHECK_MESSAGE(save_other_error == 0, "no unexpected save error under contention");

    // The saver is still in a valid state: a final save + load round-trips cleanly.
    CHECK_EQ(saver->save_changes(delta_path), OK);
    Ref<GaussianData> fresh = _make_seeded_gaussian_data(N);
    CHECK_EQ(saver->load_and_apply_changes(delta_path, fresh.ptr()), OK);

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-002j re-tracking an invalid baseline resets the splat count to 0") {
    // After tracking a valid N-splat baseline, re-tracking against a missing/corrupt
    // baseline must reset baseline_splat_count to 0 (unknown), so a delta saved afterwards
    // claims count 0 and is refused on apply (F4) -- NOT inherit the stale N count that
    // could be applied to an unrelated N-splat target (Codex).
    //
    // MUTATION that flips this case RED: make start_tracking() keep the stale count on an
    // invalid baseline (baseline_valid ? new_count : baseline_splat_count). The delta then
    // claims count N, F4 does not refuse it, and the apply mutates the N-splat target.
    const int N = 5;
    const String good_baseline = _make_persistence_fixture_path("persist002j_good", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist002j_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(good_baseline) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    // Track a valid N-splat baseline first (sets baseline_splat_count = N).
    saver->start_tracking(good_baseline);
    CHECK_EQ(saver->create_baseline(good_baseline, data.ptr()), OK);

    // Re-track against a MISSING baseline -> the count must reset to 0 (unknown).
    const String missing_baseline = _make_persistence_fixture_path("persist002j_missing", ".gsf");
    _remove_persistence_fixture(missing_baseline);
    saver->start_tracking(missing_baseline);

    // Record an edit and save; the delta must claim an unknown baseline (count 0).
    Gaussian g = data->get_gaussian(0);
    g.opacity = 0.71f;
    data->set_gaussian(0, g);
    CHECK_MESSAGE(saver->get_splat_change_count() == 1, "sanity: the edit was recorded");
    CHECK_EQ(saver->save_changes(delta_path), OK);

    // Applying to a fresh N-splat target must be REFUSED (count 0 -> F4), even though the
    // target happens to have N splats.
    Ref<GaussianData> target = _make_seeded_gaussian_data(N);
    const Error apply_err = saver->load_and_apply_changes(delta_path, target.ptr());
    CHECK_MESSAGE(apply_err == ERR_INVALID_DATA,
            "a delta saved after re-tracking an invalid baseline must be refused (unknown baseline)");
    CHECK_MESSAGE(Math::abs(target->get_gaussian(0).opacity - 0.71f) > 0.001f,
            "the refused delta must not have mutated the target");

    _remove_persistence_fixture(good_baseline);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][Persistence] PERSIST-001h set_2d_mode fails save_changes closed") {
    // is_2d_mode is outside the per-index delta contract (and the GSF baseline format cannot
    // persist it either -- #600), so set_2d_mode() must fail save_changes() closed rather
    // than silently drop the toggle on reload (Codex).
    //
    // MUTATION that flips this case RED: remove the _invalidate_incremental_delta_locked()
    // call from GaussianData::set_2d_mode(). save_changes() then returns OK with no delta.
    const int N = 4;
    const String baseline_path = _make_persistence_fixture_path("persist001h_baseline", ".gsf");
    const String delta_path = _make_persistence_fixture_path("persist001h_delta", ".gsif");
    const bool dir_ready = _ensure_persistence_fixture_dir(baseline_path) && _ensure_persistence_fixture_dir(delta_path);
    CHECK_MESSAGE(dir_ready, "Persistence fixture directory should be available");
    if (!dir_ready) {
        return;
    }

    Ref<GaussianData> data = _make_seeded_gaussian_data(N);
    Ref<GaussianSplatting::GaussianIncrementalSaver> saver;
    saver.instantiate();
    data->set_incremental_saver(saver);
    saver->start_tracking(baseline_path);
    CHECK_EQ(saver->create_baseline(baseline_path, data.ptr()), OK);
    CHECK_MESSAGE(!saver->get_requires_full_save(), "sanity: clean baseline, guard clear");

    data->set_2d_mode(true);
    CHECK_MESSAGE(saver->get_requires_full_save(), "set_2d_mode() must invalidate the delta");
    CHECK_MESSAGE(saver->save_changes(delta_path) == ERR_UNAVAILABLE,
            "save_changes() must fail closed rather than drop the 2D-mode toggle");

    _remove_persistence_fixture(baseline_path);
    _remove_persistence_fixture(delta_path);
}

TEST_CASE("[GaussianSplatting][WorldLifetime] GaussianSplatWorld::clear() drops chunk_payload_source") {
    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK(data.is_valid());

    Ref<InMemoryChunkPayloadSource> payload_source;
    payload_source.instantiate();
    payload_source->set_data(data);

    world->set_chunk_payload_source(payload_source);
    CHECK_MESSAGE(world->get_chunk_payload_source().is_valid(),
            "Sanity: payload source should be attached before clear()");

    world->clear();

    CHECK_MESSAGE(world->get_gaussian_data().is_null(),
            "clear() must drop gaussian_data");
    CHECK_MESSAGE(world->get_chunk_payload_source().is_null(),
            "clear() must drop chunk_payload_source");
}

TEST_CASE("[GaussianSplatting][Persistence] validate_file accepts valid GSF") {
    const String path = _make_persistence_fixture_path("test_validate");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer serializer;
    Error save_err = serializer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save should succeed");

    if (save_err != OK) return;

    Error validate_err = serializer.validate_file(path);
    CHECK_MESSAGE(validate_err == OK, "validate_file should accept valid GSF");

    bool is_gsf = GaussianSplatting::GaussianSceneSerializer::is_gaussian_scene_file(path);
    CHECK_MESSAGE(is_gsf, "is_gaussian_scene_file should accept valid chunked GSF");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] load_scene accepts forward-compatible future versions") {
    const String path = _make_persistence_fixture_path("test_forward_compatible_version");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer serializer;
    serializer.set_enable_checksum(false);
    Error save_err = serializer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save should succeed");
    if (save_err != OK) {
        return;
    }

    const uint16_t future_version = GaussianSplatting::GAUSSIAN_SCENE_VERSION + 1;
    const bool patched = _overwrite_scene_header_versions(path, future_version, GaussianSplatting::GAUSSIAN_SCENE_VERSION);
    CHECK_MESSAGE(patched, "Fixture header should be patchable for forward-compatibility test");
    if (!patched) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == OK, "Forward-compatible future version should load successfully");
    CHECK_MESSAGE(loaded_data->get_count() == data->get_count(),
            "Forward-compatible load should preserve splat count");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] load_scene rejects forward-incompatible future versions") {
    const String path = _make_persistence_fixture_path("test_forward_incompatible_version");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer serializer;
    serializer.set_enable_checksum(false);
    Error save_err = serializer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save should succeed");
    if (save_err != OK) {
        return;
    }

    const uint16_t future_version = GaussianSplatting::GAUSSIAN_SCENE_VERSION + 1;
    const uint16_t incompatible_reader_floor = GaussianSplatting::GAUSSIAN_SCENE_VERSION + 1;
    const bool patched = _overwrite_scene_header_versions(path, future_version, incompatible_reader_floor);
    CHECK_MESSAGE(patched, "Fixture header should be patchable for forward-incompatibility test");
    if (!patched) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == ERR_FILE_UNRECOGNIZED,
            "Forward-incompatible future version should be rejected");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] unknown chunks round-trip across load and save") {
    const String path = _make_persistence_fixture_path("test_unknown_chunk_roundtrip");
    const String resaved_path = _make_persistence_fixture_path("test_unknown_chunk_roundtrip_resave");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path) && _ensure_persistence_fixture_dir(resaved_path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    Dictionary metadata;
    metadata[StringName("roundtrip_probe")] = true;

    GaussianSplatting::GaussianSceneSerializer serializer;
    Error save_err = serializer.save_scene(path, data.ptr(), nullptr, metadata);
    CHECK_MESSAGE(save_err == OK, "GSF save with metadata should succeed");
    if (save_err != OK) {
        _remove_persistence_fixture(path);
        _remove_persistence_fixture(resaved_path);
        return;
    }

    const uint32_t unknown_chunk_type = 0x554E4B4Eu; // "UNKN"
    const bool retagged = _retag_first_metadata_chunk_as_unknown(path, unknown_chunk_type);
    CHECK_MESSAGE(retagged, "Fixture should contain a metadata chunk to retag");
    if (!retagged) {
        _remove_persistence_fixture(path);
        _remove_persistence_fixture(resaved_path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    Dictionary loaded_metadata;
    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, &loaded_metadata);
    CHECK_MESSAGE(load_err == OK, "Loading fixture with unknown chunk should succeed");
    CHECK_MESSAGE(serializer.get_unknown_chunk_count() == 1,
            "Serializer should preserve exactly one unknown chunk for round-trip");

    Error resave_err = serializer.save_scene(resaved_path, loaded_data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(resave_err == OK, "Resaving after unknown chunk load should succeed");
    if (resave_err == OK) {
        CHECK_MESSAGE(_file_contains_chunk_type(resaved_path, unknown_chunk_type),
                "Resaved file should still contain preserved unknown chunk type");
    }

    _remove_persistence_fixture(path);
    _remove_persistence_fixture(resaved_path);
}

// #700 -- structural acceptance gaps that let a file the writer cannot produce
// validate and load as OK.
//
// All three cases assert on BOTH validate_file() and load_scene(). #618 made
// those two agree by construction, and a rejection that only one of them makes
// is the exact asymmetry that was fixed there; asserting one would not notice
// it coming back.
TEST_CASE("[GaussianSplatting][Persistence] EOF chunk declaring a payload is rejected") {
    const String path = _make_persistence_fixture_path("test_eof_declares_payload");
    if (!_ensure_persistence_fixture_dir(path)) {
        FAIL("persistence fixture directory unavailable; nothing can be written to tamper with");
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    if (data.is_null()) {
        FAIL("test world produced no gaussian data; the fixture would be empty");
        return;
    }

    GaussianSplatting::GaussianSceneSerializer serializer;
    if (serializer.save_scene(path, data.ptr(), nullptr, Dictionary()) != OK) {
        _remove_persistence_fixture(path);
        FAIL("GSF save failed; there is no valid fixture to tamper with");
        return;
    }

    // Sanity: the untampered fixture must load, or a later rejection would
    // prove nothing about the tamper.
    Ref<GaussianData> baseline_data;
    baseline_data.instantiate();
    CHECK_MESSAGE(serializer.load_scene(path, baseline_data.ptr(), nullptr, nullptr) == OK,
            "the untampered fixture must load, or the rejection below is not attributable to the tamper");

    // Append the 8 bytes the terminator will claim, so the payload FITS inside
    // the file. Without them the pre-existing bounds check at the top of the
    // chunk loop ("payload extends past end of file") rejects the fixture and
    // this case would pass without ever reaching the rule it is meant to pin.
    PackedByteArray declared_payload;
    declared_payload.resize(8);
    for (int i = 0; i < declared_payload.size(); i++) {
        declared_payload.set(i, uint8_t(0x5Au));
    }
    const bool payload_appended = _append_fixture_tail(path, declared_payload);
    CHECK_MESSAGE(payload_appended, "fixture should be appendable");
    const bool patched = payload_appended && _set_eof_chunk_declared_size(path, 8);
    CHECK_MESSAGE(patched, "fixture should contain an END_OF_FILE chunk to patch");
    if (!patched) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    CHECK_MESSAGE(serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr) == ERR_FILE_CORRUPT,
            "an END_OF_FILE chunk declaring a payload is not writer-producible and must be rejected");
    CHECK_MESSAGE(serializer.validate_file(path) == ERR_FILE_CORRUPT,
            "validate_file must reject what load_scene rejects (it returns Error, not bool -- "
            "`!validate_file(...)` would assert the OPPOSITE and pass on a broken tree)");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] Bytes appended after the EOF chunk are rejected") {
    const String path = _make_persistence_fixture_path("test_trailing_bytes_after_eof");
    if (!_ensure_persistence_fixture_dir(path)) {
        FAIL("persistence fixture directory unavailable; nothing can be written to tamper with");
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    if (data.is_null()) {
        FAIL("test world produced no gaussian data; the fixture would be empty");
        return;
    }

    GaussianSplatting::GaussianSceneSerializer serializer;
    if (serializer.save_scene(path, data.ptr(), nullptr, Dictionary()) != OK) {
        _remove_persistence_fixture(path);
        FAIL("GSF save failed; there is no valid fixture to tamper with");
        return;
    }

    Ref<GaussianData> baseline_data;
    baseline_data.instantiate();
    CHECK_MESSAGE(serializer.load_scene(path, baseline_data.ptr(), nullptr, nullptr) == OK,
            "the untampered fixture must load, or the rejection below is not attributable to the appended bytes");

    PackedByteArray tail;
    tail.resize(16);
    for (int i = 0; i < tail.size(); i++) {
        tail.set(i, uint8_t(0xA5));
    }
    const bool appended = _append_fixture_tail(path, tail);
    CHECK_MESSAGE(appended, "fixture should be appendable");
    if (!appended) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    CHECK_MESSAGE(serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr) == ERR_FILE_CORRUPT,
            "the chunk loop stops at the terminator, so appended bytes are never parsed, never counted "
            "against header.total_chunks and never checksummed -- the reader must reject them outright");
    CHECK_MESSAGE(serializer.validate_file(path) == ERR_FILE_CORRUPT,
            "validate_file must reject what load_scene rejects (it returns Error, not bool -- "
            "`!validate_file(...)` would assert the OPPOSITE and pass on a broken tree)");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] Tampered unknown-chunk payload fails checksum verification") {
    const String path = _make_persistence_fixture_path("test_unknown_chunk_tamper");
    if (!_ensure_persistence_fixture_dir(path)) {
        FAIL("persistence fixture directory unavailable; nothing can be written to tamper with");
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    if (data.is_null()) {
        FAIL("test world produced no gaussian data; the fixture would be empty");
        return;
    }

    Dictionary metadata;
    metadata[StringName("tamper_probe")] = true;

    // Checksums ON: this case is about the verification step, so the file must
    // claim protection. (_verify_checksum's policy is that a checksum-disabled
    // reader still verifies a file that claims protection -- #618.)
    GaussianSplatting::GaussianSceneSerializer serializer;
    serializer.set_enable_checksum(true);
    if (serializer.save_scene(path, data.ptr(), nullptr, metadata) != OK) {
        _remove_persistence_fixture(path);
        FAIL("GSF save failed; there is no valid fixture to tamper with");
        return;
    }

    const uint32_t unknown_chunk_type = 0x554E4B4Eu; // "UNKN"
    const bool retagged = _retag_first_metadata_chunk_as_unknown(path, unknown_chunk_type);
    CHECK_MESSAGE(retagged, "fixture should contain a metadata chunk to retag as unknown");
    if (!retagged) {
        _remove_persistence_fixture(path);
        return;
    }

    // A retagged-but-untampered unknown chunk must still load: forward
    // compatibility is the point of the branch, and without this the rejection
    // below could be the retag rather than the tamper.
    Ref<GaussianData> preserved_data;
    preserved_data.instantiate();
    CHECK_MESSAGE(serializer.load_scene(path, preserved_data.ptr(), nullptr, nullptr) == OK,
            "an intact unknown chunk must still round-trip; the new check must not reject forward-compatible data");

    const bool tampered = _tamper_chunk_payload_byte(path, unknown_chunk_type);
    CHECK_MESSAGE(tampered, "unknown chunk should carry a payload byte to flip");
    if (!tampered) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    CHECK_MESSAGE(serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr) == ERR_FILE_CORRUPT,
            "a tampered unknown-chunk payload must fail the same checksum policy every known chunk uses; "
            "accepting it also LAUNDERS the corruption, because re-save writes the original checksum back "
            "over the tampered bytes");
    CHECK_MESSAGE(serializer.validate_file(path) == ERR_FILE_CORRUPT,
            "validate_file must reject what load_scene rejects (it returns Error, not bool -- "
            "`!validate_file(...)` would assert the OPPOSITE and pass on a broken tree)");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] Zero-size unknown chunk with a bad checksum is rejected") {
    // Codex PR #718: the unknown-chunk verification added by #700 sat INSIDE the
    // `if (chunk.size > 0)` branch, so a zero-size unknown chunk was never
    // verified at all. One carrying a non-zero checksum therefore loaded clean
    // and was preserved, and the re-save path re-emitted that checksum -- the
    // exact laundering the check exists to prevent, reachable by declaring
    // size 0. Every KNOWN chunk already rejects this shape: their readers call
    // _verify_checksum unconditionally, and the checksum of empty data is
    // defined as 0.
    const String path = _make_persistence_fixture_path("test_unknown_chunk_empty_bad_checksum");
    if (!_ensure_persistence_fixture_dir(path)) {
        FAIL("persistence fixture directory unavailable; nothing can be written to tamper with");
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    if (data.is_null()) {
        FAIL("test world produced no gaussian data; the fixture would be empty");
        return;
    }

    Dictionary metadata;
    metadata[StringName("empty_unknown_probe")] = true;

    // Checksums ON: the file must CLAIM protection or _verify_checksum's #618
    // policy short-circuits to true and this case proves nothing.
    GaussianSplatting::GaussianSceneSerializer serializer;
    serializer.set_enable_checksum(true);
    if (serializer.save_scene(path, data.ptr(), nullptr, metadata) != OK) {
        _remove_persistence_fixture(path);
        FAIL("GSF save failed; there is no valid fixture to tamper with");
        return;
    }

    const uint32_t unknown_chunk_type = 0x554E4B45u; // "UNKE"

    // Control FIRST, on its own fixture: a zero-size unknown chunk whose
    // checksum is the correct 0 must still LOAD. Without this the rejection
    // below could be the empty payload, the retag, the chunk-count check or the
    // terminator-position check rather than the checksum rule -- green for the
    // wrong reason, which is the trap #718's earlier cases were each held to.
    const String control_path = _make_persistence_fixture_path("test_unknown_chunk_empty_good_checksum");
    if (serializer.save_scene(control_path, data.ptr(), nullptr, metadata) != OK) {
        _remove_persistence_fixture(path);
        _remove_persistence_fixture(control_path);
        FAIL("GSF save failed for the control fixture");
        return;
    }
    const bool control_rewritten =
            _replace_first_metadata_chunk_with_empty_unknown(control_path, unknown_chunk_type, 0u);
    CHECK_MESSAGE(control_rewritten, "control fixture should contain a metadata chunk to replace");
    if (control_rewritten) {
        Ref<GaussianData> control_data;
        control_data.instantiate();
        CHECK_MESSAGE(serializer.load_scene(control_path, control_data.ptr(), nullptr, nullptr) == OK,
                "a zero-size unknown chunk with the correct empty checksum (0) must still load -- "
                "forward compatibility is the whole point of the unknown-chunk branch, and this "
                "control is what proves the rejection below is the CHECKSUM and not the shape");
        CHECK_MESSAGE(serializer.validate_file(control_path) == OK,
                "validate_file must accept what load_scene accepts (it returns Error, not bool -- "
                "`!validate_file(...)` would assert the OPPOSITE and pass on a broken tree)");
    }
    _remove_persistence_fixture(control_path);

    // The case under test: identical shape, non-zero checksum over no bytes.
    const bool rewritten =
            _replace_first_metadata_chunk_with_empty_unknown(path, unknown_chunk_type, 0xDEADBEEFu);
    CHECK_MESSAGE(rewritten, "fixture should contain a metadata chunk to replace");
    if (!rewritten) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    CHECK_MESSAGE(serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr) == ERR_FILE_CORRUPT,
            "a zero-size unknown chunk whose checksum is not the empty-payload checksum (0) must be "
            "rejected exactly as a known chunk in that shape is; accepting it also LAUNDERS the "
            "mismatch, because re-save writes that checksum back out over the empty payload");
    CHECK_MESSAGE(serializer.validate_file(path) == ERR_FILE_CORRUPT,
            "validate_file must reject what load_scene rejects (it returns Error, not bool -- "
            "`!validate_file(...)` would assert the OPPOSITE and pass on a broken tree)");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] Checksum-mode migration re-checksums preserved unknown chunks") {
    // Second-order regression from Codex PR #718. #700/#718 taught the READ path
    // to verify unknown chunks under the same checksum policy as every known
    // chunk. But the WRITE path re-emitted each preserved unknown chunk's
    // checksum VERBATIM. Load a forward-compatible file with checksums OFF and
    // the preserved unknown chunk keeps checksum 0 (an unprotected source stores
    // 0 over its payload); re-save with checksums ON and the known chunks get
    // real checksums while the unknown chunk still carries 0 over a NON-EMPTY
    // payload. The now-protected file then fails the very verification #718
    // added -- the serializer rejects its OWN output, so checksum-mode migration
    // of forward-compatible files was lossy. The fix recomputes the unknown
    // chunk's checksum on write, exactly as known chunks are handled.
    const String path = _make_persistence_fixture_path("test_unknown_chunk_migrate_src");
    const String resaved_path = _make_persistence_fixture_path("test_unknown_chunk_migrate_dst");
    const bool fixture_dir_ready =
            _ensure_persistence_fixture_dir(path) && _ensure_persistence_fixture_dir(resaved_path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    if (data.is_null()) {
        FAIL("test world produced no gaussian data; the fixture would be empty");
        return;
    }

    Dictionary metadata;
    metadata[StringName("migrate_probe")] = true;

    // Source written WITHOUT checksums: it claims no protection, so its metadata
    // chunk -- and therefore the unknown chunk we retag it into -- stores
    // checksum 0 over a NON-EMPTY payload, the exact shape a verbatim re-emit
    // gets wrong once the file is marked protected.
    GaussianSplatting::GaussianSceneSerializer serializer;
    serializer.set_enable_checksum(false);
    if (serializer.save_scene(path, data.ptr(), nullptr, metadata) != OK) {
        _remove_persistence_fixture(path);
        _remove_persistence_fixture(resaved_path);
        FAIL("checksum-disabled GSF save failed; there is no valid fixture to migrate");
        return;
    }

    const uint32_t unknown_chunk_type = 0x4D494752u; // "MIGR"
    const bool retagged = _retag_first_metadata_chunk_as_unknown(path, unknown_chunk_type);
    CHECK_MESSAGE(retagged, "fixture should contain a metadata chunk to retag as unknown");
    if (!retagged) {
        _remove_persistence_fixture(path);
        _remove_persistence_fixture(resaved_path);
        return;
    }

    // Load with checksums OFF: forward-compatible, unprotected. The preserved
    // unknown chunk keeps checksum 0.
    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    const Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == OK, "checksum-disabled load of a forward-compatible file must succeed");
    CHECK_MESSAGE(serializer.get_unknown_chunk_count() == 1,
            "exactly one unknown chunk must be preserved for the migration to be meaningful");
    if (load_err != OK || serializer.get_unknown_chunk_count() != 1) {
        _remove_persistence_fixture(path);
        _remove_persistence_fixture(resaved_path);
        return;
    }

    // Migrate: re-save the SAME instance (it carries the preserved unknown chunk)
    // with checksums ON. The re-saved file is produced by the serializer itself,
    // so it is structurally valid by construction -- #601/#700's chunk-count and
    // terminator-position checks cannot fire -- and the ONLY thing that can make
    // validation reject it is the unknown chunk's checksum, precisely the path
    // under test (green-for-the-wrong-reason is ruled out by construction).
    serializer.set_enable_checksum(true);
    const Error resave_err = serializer.save_scene(resaved_path, loaded_data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(resave_err == OK, "re-save with checksums enabled must succeed");
    if (resave_err != OK) {
        _remove_persistence_fixture(path);
        _remove_persistence_fixture(resaved_path);
        return;
    }
    CHECK_MESSAGE(_file_contains_chunk_type(resaved_path, unknown_chunk_type),
            "the preserved unknown chunk must survive into the migrated file, or validation below is vacuous");

    // Decisive assertion (FAILS on the pre-fix #718 tree): the serializer must
    // accept the file it just produced. validate_file() returns Error, not bool,
    // so this is compared to OK explicitly -- `!validate_file(...)` would assert
    // the opposite and pass on the broken tree.
    GaussianSplatting::GaussianSceneSerializer verifier; // default: checksums ON.
    CHECK_MESSAGE(verifier.validate_file(resaved_path) == OK,
            "checksum-mode migration must produce a file the serializer accepts; the unknown chunk's "
            "checksum must be recomputed on write, not re-emitted from a checksum-disabled load");

    // ...and a full reload with checksums on must round-trip the unknown chunk.
    Ref<GaussianData> reloaded_data;
    reloaded_data.instantiate();
    CHECK_MESSAGE(verifier.load_scene(resaved_path, reloaded_data.ptr(), nullptr, nullptr) == OK,
            "the migrated file must fully reload with checksums enabled");
    CHECK_MESSAGE(verifier.get_unknown_chunk_count() == 1,
            "the unknown chunk must survive the checksum-mode migration round-trip");

    // Control -- proves the write-side fix did NOT disable #700/#718's read-side
    // verification: flip a payload byte of the migrated (now correctly
    // checksummed) unknown chunk WITHOUT updating its checksum. Loading and
    // validating with checksums on must still reject it as corrupt.
    const bool tampered = _tamper_chunk_payload_byte(resaved_path, unknown_chunk_type);
    CHECK_MESSAGE(tampered, "migrated unknown chunk should carry a payload byte to flip for the control");
    if (tampered) {
        Ref<GaussianData> corrupt_data;
        corrupt_data.instantiate();
        CHECK_MESSAGE(verifier.load_scene(resaved_path, corrupt_data.ptr(), nullptr, nullptr) == ERR_FILE_CORRUPT,
                "a tampered unknown-chunk payload must still be rejected -- the write-side fix must not "
                "weaken the #700/#718 read-side verification");
        CHECK_MESSAGE(verifier.validate_file(resaved_path) == ERR_FILE_CORRUPT,
                "validate_file must reject what load_scene rejects (it returns Error, not bool -- "
                "`!validate_file(...)` would assert the OPPOSITE and pass on a broken tree)");
    }

    _remove_persistence_fixture(path);
    _remove_persistence_fixture(resaved_path);
}

TEST_CASE("[GaussianSplatting][Persistence] Validation helpers accept chunked GSF without checksums") {
    const String path = _make_persistence_fixture_path("test_validate_no_checksum");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_enable_checksum(false);
    Error save_err = writer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save without checksums should succeed");

    if (save_err != OK) return;

    GaussianSplatting::GaussianSceneSerializer strict_validator;
    Error strict_validate_err = strict_validator.validate_file(path);
    CHECK_MESSAGE(strict_validate_err == ERR_FILE_CORRUPT,
            "Default validate_file should reject checksum-disabled chunked GSF");
    CHECK_FALSE_MESSAGE(
            GaussianSplatting::GaussianSceneSerializer::is_gaussian_scene_file(path),
            "Default is_gaussian_scene_file should reject checksum-disabled chunked GSF");

    GaussianSplatting::GaussianSceneSerializer legacy_validator;
    legacy_validator.set_enable_checksum(false);
    Error legacy_validate_err = legacy_validator.validate_file(path);
    CHECK_MESSAGE(legacy_validate_err == OK,
            "validate_file should accept checksum-disabled chunked GSF when checksum validation is explicitly disabled");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Validation rejects checksum-stripped protected chunked GSF") {
    const String path = _make_persistence_fixture_path("test_validate_checksum_stripped_protected");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_enable_checksum(true);
    Error save_err = writer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save with checksums should succeed");
    if (save_err != OK) {
        return;
    }

    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::READ_WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to mutate checksum-protected GSF fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }
    const uint64_t file_length = file->get_length();
    bool saw_eof_chunk = false;
    file->seek(0);
    while (file->get_position() + uint64_t(sizeof(GaussianSplatting::ChunkHeader)) <= file_length) {
        const uint32_t chunk_type_raw = file->get_32();
        const uint32_t chunk_size = file->get_32();
        file->store_32(0); // Zero every chunk checksum field.
        file->get_32(); // chunk flags
        const uint64_t payload_offset = file->get_position();

        if (chunk_type_raw == uint32_t(GaussianSplatting::ChunkType::HEADER)) {
            const uint64_t scene_flags_offset =
                    payload_offset + sizeof(uint32_t) + sizeof(uint16_t);
            CHECK_MESSAGE(scene_flags_offset + sizeof(uint16_t) <= file_length,
                    "Fixture should contain a full scene header flags field");
            if (!(scene_flags_offset + sizeof(uint16_t) <= file_length)) {
                file.unref();
                _remove_persistence_fixture(path);
                return;
            }
            file->seek(scene_flags_offset);
            const uint16_t scene_flags = file->get_16();
            file->seek(scene_flags_offset);
            file->store_16(scene_flags & ~uint16_t(1u << 1));
            file->seek(payload_offset);
        }

        if (chunk_type_raw == uint32_t(GaussianSplatting::ChunkType::END_OF_FILE)) {
            saw_eof_chunk = true;
            break;
        }
        CHECK_MESSAGE(payload_offset + uint64_t(chunk_size) <= file_length,
                "Fixture chunk payload should stay within file bounds");
        if (!(payload_offset + uint64_t(chunk_size) <= file_length)) {
            file.unref();
            _remove_persistence_fixture(path);
            return;
        }
        file->seek(payload_offset + uint64_t(chunk_size));
    }
    CHECK_MESSAGE(saw_eof_chunk, "Fixture should include an END_OF_FILE chunk");
    if (!saw_eof_chunk) {
        file.unref();
        _remove_persistence_fixture(path);
        return;
    }
    file.unref();

    GaussianSplatting::GaussianSceneSerializer validator;
    Error validate_err = validator.validate_file(path);
    CHECK_MESSAGE(validate_err == ERR_FILE_CORRUPT,
            "validate_file should reject checksum-stripped files that were originally checksum-protected");
    CHECK_FALSE_MESSAGE(
            GaussianSplatting::GaussianSceneSerializer::is_gaussian_scene_file(path),
            "is_gaussian_scene_file should reject checksum-stripped files that were originally checksum-protected");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Validation rejects checksum-tampered chunked GSF") {
    const String path = _make_persistence_fixture_path("test_validate_checksum_tampered");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_enable_checksum(true);
    Error save_err = writer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save with checksums should succeed");
    if (save_err != OK) {
        return;
    }

    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::READ_WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to mutate checksum-protected GSF fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }
    const uint64_t payload_offset = uint64_t(sizeof(GaussianSplatting::ChunkHeader));
    CHECK_MESSAGE(file->get_length() > payload_offset, "Fixture should contain a header payload");
    if (!(file->get_length() > payload_offset)) {
        file.unref();
        _remove_persistence_fixture(path);
        return;
    }
    file->seek(payload_offset);
    const uint8_t original_byte = file->get_8();
    file->seek(payload_offset);
    file->store_8(original_byte ^ 0x01); // Tamper payload without updating checksum.
    file.unref();

    GaussianSplatting::GaussianSceneSerializer validator;
    Error validate_err = validator.validate_file(path);
    CHECK_MESSAGE(validate_err == ERR_FILE_CORRUPT,
            "validate_file should reject checksum-tampered chunked GSF");
    CHECK_FALSE_MESSAGE(
            GaussianSplatting::GaussianSceneSerializer::is_gaussian_scene_file(path),
            "is_gaussian_scene_file should reject checksum-tampered chunked GSF");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Validation rejects checksum-zeroed protected headers") {
    const String path = _make_persistence_fixture_path("test_validate_zeroed_header_checksum");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_enable_checksum(true);
    Error save_err = writer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save with checksums should succeed");
    if (save_err != OK) {
        return;
    }

    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::READ_WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to mutate checksum-protected GSF fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }
    file->seek(8); // Chunk header checksum field in HEAD chunk.
    file->store_32(0); // Zero out checksum field without changing payload.
    file.unref();

    GaussianSplatting::GaussianSceneSerializer validator;
    Error validate_err = validator.validate_file(path);
    CHECK_MESSAGE(validate_err == ERR_FILE_CORRUPT,
            "validate_file should reject checksum-protected headers with zeroed checksum fields");
    CHECK_FALSE_MESSAGE(
            GaussianSplatting::GaussianSceneSerializer::is_gaussian_scene_file(path),
            "is_gaussian_scene_file should reject checksum-protected headers with zeroed checksum fields");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Validation rejects checksum-zeroed legacy checksummed headers") {
    const String path = _make_persistence_fixture_path("test_validate_zeroed_legacy_header_checksum");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_enable_checksum(true);
    Error save_err = writer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save with checksums should succeed");
    if (save_err != OK) {
        return;
    }

    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::READ_WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to mutate checksum-protected GSF fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    // Zero out the HEAD chunk checksum field.
    file->seek(8);
    file->store_32(0);

    // Clear checksum-enabled scene flag to emulate older checksummed files.
    const uint64_t flags_offset =
            uint64_t(sizeof(GaussianSplatting::ChunkHeader)) + sizeof(uint32_t) + sizeof(uint16_t);
    file->seek(flags_offset);
    uint16_t header_flags = file->get_16();
    file->seek(flags_offset);
    file->store_16(header_flags & ~uint16_t(1u << 1));

    // Ensure at least one trailing chunk still advertises a checksum.
    const uint64_t second_chunk_checksum_offset =
            uint64_t(sizeof(GaussianSplatting::ChunkHeader)) + GaussianSplatting::SCENE_HEADER_PACKED_SIZE + 8;
    file->seek(second_chunk_checksum_offset);
    const uint32_t trailing_checksum = file->get_32();
    CHECK_MESSAGE(trailing_checksum != 0, "Fixture should preserve non-zero trailing chunk checksums");
    if (trailing_checksum == 0) {
        file.unref();
        _remove_persistence_fixture(path);
        return;
    }
    file.unref();

    GaussianSplatting::GaussianSceneSerializer validator;
    Error validate_err = validator.validate_file(path);
    CHECK_MESSAGE(validate_err == ERR_FILE_CORRUPT,
            "validate_file should reject legacy checksum-protected headers with zeroed checksum fields");
    CHECK_FALSE_MESSAGE(
            GaussianSplatting::GaussianSceneSerializer::is_gaussian_scene_file(path),
            "is_gaussian_scene_file should reject legacy checksum-protected headers with zeroed checksum fields");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Validation rejects non-chunked magic-at-byte0 payloads") {
    const String path = _make_persistence_fixture_path("test_validate_invalid_chunking");
    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to create invalid GSF test fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    // Malformed payload: scene magic appears at byte 0 instead of inside HEAD chunk payload.
    file->store_32(GaussianSplatting::GAUSSIAN_SCENE_MAGIC);
    file->store_32(0);
    file->store_32(0);
    file->store_32(0);
    file.unref();

    GaussianSplatting::GaussianSceneSerializer serializer;
    Error validate_err = serializer.validate_file(path);
    CHECK_MESSAGE(validate_err != OK, "validate_file must reject malformed non-chunked payloads");
    CHECK_FALSE_MESSAGE(
            GaussianSplatting::GaussianSceneSerializer::is_gaussian_scene_file(path),
            "is_gaussian_scene_file must reject malformed non-chunked payloads");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Validation rejects truncated chunked header payloads") {
    const String path = _make_persistence_fixture_path("test_validate_truncated_header_chunk");
    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to create truncated chunked fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    // Chunked container shape exists, but HEAD payload bytes are missing.
    file->store_32((uint32_t)GaussianSplatting::ChunkType::HEADER);
    file->store_32(GaussianSplatting::SCENE_HEADER_PACKED_SIZE);
    file->store_32(0);
    file->store_32(0);
    file.unref();

    GaussianSplatting::GaussianSceneSerializer serializer;
    Error validate_err = serializer.validate_file(path);
    CHECK_MESSAGE(validate_err != OK, "validate_file must reject truncated chunked header payloads");
    CHECK_FALSE_MESSAGE(
            GaussianSplatting::GaussianSceneSerializer::is_gaussian_scene_file(path),
            "is_gaussian_scene_file must reject truncated chunked header payloads");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Incremental loader rejects malformed change tables") {
    const String path = _make_persistence_fixture_path("test_incremental_malformed_table", ".gsif");
    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to create malformed incremental fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    file->store_32(GaussianSplatting::INCREMENTAL_MAGIC);
    file->store_16(GaussianSplatting::INCREMENTAL_VERSION);
    file->store_16(0);
    file->store_64(1);
    file->store_64(0);
    file->store_32(0);
    file->store_32(0xFFFFFFFF); // Unreasonably large untrusted change_count.
    file.unref();

    GaussianSplatting::GaussianIncrementalSaver saver;
    Error err = saver.load_and_apply_changes(path, nullptr, nullptr);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT, "Malformed change table should be rejected as corrupt");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Incremental loader rejects truncated change table header") {
    const String path = _make_persistence_fixture_path("test_incremental_truncated_header", ".gsif");
    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to create truncated incremental fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    file->store_32(GaussianSplatting::INCREMENTAL_MAGIC);
    file->store_16(GaussianSplatting::INCREMENTAL_VERSION);
    file->store_16(0);
    // Intentionally stop before writing timestamps/counts.
    file.unref();

    GaussianSplatting::GaussianIncrementalSaver saver;
    Error err = saver.load_and_apply_changes(path, nullptr, nullptr);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT, "Truncated change table header should be rejected as corrupt");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Incremental loader rejects out-of-range payload slices") {
    const String path = _make_persistence_fixture_path("test_incremental_oob_payload", ".gsif");
    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to create OOB incremental fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    file->store_32(GaussianSplatting::INCREMENTAL_MAGIC);
    file->store_16(GaussianSplatting::INCREMENTAL_VERSION);
    file->store_16(0);
    file->store_64(2);
    file->store_64(0);
    file->store_32(0);
    file->store_32(1); // change_count

    file->store_8((uint8_t)GaussianSplatting::ChangeType::SPLAT_MODIFIED);
    file->store_32(1024); // data_offset points beyond payload
    file->store_32(16);
    file->store_64(2);

    PackedByteArray tiny_payload;
    tiny_payload.resize(8);
    file->store_buffer(tiny_payload);
    file.unref();

    GaussianSplatting::GaussianIncrementalSaver saver;
    Error err = saver.load_and_apply_changes(path, nullptr, nullptr);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT, "Out-of-range payload slice should be rejected as corrupt");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Incremental loader rejects overflow-sized payload slices") {
    const String path = _make_persistence_fixture_path("test_incremental_overflow_payload_slice", ".gsif");
    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to create overflow incremental fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    file->store_32(GaussianSplatting::INCREMENTAL_MAGIC);
    file->store_16(GaussianSplatting::INCREMENTAL_VERSION);
    file->store_16(0);
    file->store_64(3);
    file->store_64(0);
    file->store_32(0);
    file->store_32(1); // change_count

    file->store_8((uint8_t)GaussianSplatting::ChangeType::SPLAT_MODIFIED);
    file->store_32(32); // data_offset inside payload
    file->store_32(0xFFFFFFFF); // data_size overflows 32-bit addition in unsafe parsers
    file->store_64(3);

    PackedByteArray payload;
    payload.resize(64);
    file->store_buffer(payload);
    file.unref();

    GaussianSplatting::GaussianIncrementalSaver saver;
    Error err = saver.load_and_apply_changes(path, nullptr, nullptr);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT, "Overflow-sized payload slices should be rejected as corrupt");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] load_scene rejects a scene truncated before END_OF_FILE") {
    // Transactional-load regression (#601): a scene whose END_OF_FILE chunk was
    // lost (e.g. a partial/truncated write) MUST fail to load rather than silently
    // returning OK with a half-populated target. The pre-fix loader exits the
    // chunk loop on stream-EOF and returns OK, so this case fails on base.
    const String path = _make_persistence_fixture_path("test_truncated_before_eof");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer serializer;
    Error save_err = serializer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save should succeed");
    if (save_err != OK) {
        _remove_persistence_fixture(path);
        return;
    }

    // Drop exactly the 16-byte END_OF_FILE chunk (the final chunk the writer
    // emits). Every earlier chunk -- including its valid checksum -- stays intact,
    // so ONLY the structural terminator is missing.
    const bool truncated = _truncate_fixture_tail(path, sizeof(GaussianSplatting::ChunkHeader));
    CHECK_MESSAGE(truncated, "Fixture should be truncatable");
    if (!truncated) {
        _remove_persistence_fixture(path);
        return;
    }

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT,
            "load_scene must reject a scene truncated before its END_OF_FILE chunk");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] validate_file rejects a header-valid scene truncated before END_OF_FILE") {
    // Validation-parity regression (#602): the old validate_file trusted the
    // header alone on the checksum-success path and returned OK for a file whose
    // trailing chunks are missing. It must now run the real body parse, so any
    // file that fails load_scene also fails validate_file.
    const String path = _make_persistence_fixture_path("test_validate_truncated_before_eof");
    const bool fixture_dir_ready = _ensure_persistence_fixture_dir(path);
    CHECK_MESSAGE(fixture_dir_ready, "Persistence fixture directory should be available");
    if (!fixture_dir_ready) {
        return;
    }

    Ref<GaussianSplatWorld> world = create_test_world();
    Ref<GaussianData> data = world->get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Test data should be valid");

    GaussianSplatting::GaussianSceneSerializer serializer;
    Error save_err = serializer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "GSF save should succeed");
    if (save_err != OK) {
        _remove_persistence_fixture(path);
        return;
    }

    const bool truncated = _truncate_fixture_tail(path, sizeof(GaussianSplatting::ChunkHeader));
    CHECK_MESSAGE(truncated, "Fixture should be truncatable");
    if (!truncated) {
        _remove_persistence_fixture(path);
        return;
    }

    // The HEAD chunk is still structurally intact, so a header-only probe
    // (get_file_info) still reports the file as valid -- this is exactly what the
    // old validate_file trusted on the checksum-success path.
    Dictionary info = serializer.get_file_info(path);
    CHECK_MESSAGE(bool(info.get("valid", false)),
            "The intact header alone still parses (the old validate_file's blind spot)");

    Error validate_err = serializer.validate_file(path);
    CHECK_MESSAGE(validate_err == ERR_FILE_CORRUPT,
            "validate_file must reject a scene whose body is truncated before END_OF_FILE");

    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT,
            "load_scene rejects the same file -- validate_file and load_scene now agree");
    CHECK_FALSE_MESSAGE(
            GaussianSplatting::GaussianSceneSerializer::is_gaussian_scene_file(path),
            "is_gaussian_scene_file must also reject the truncated scene");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] load_scene rejects a decompress-bomb chunk (hostile ratio)") {
    // #603a: a compressed GAUSSIAN_DATA chunk whose declared decompressed size is
    // wildly out of proportion to its actual compressed bytes (a decompress bomb)
    // must be rejected BEFORE the multi-GiB allocation. It is refused twice over:
    // 8 compressed bytes cannot physically emit ~4 GiB under Zstd, and the scene
    // header corroborates only 1 splat. The guard is NOT an absolute INT32_MAX cap
    // (see the boundary test below).
    const String path = _make_persistence_fixture_path("test_oversized_original_size");
    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to create oversized-chunk fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    _write_gsf_header_chunk(file, /*total_chunks=*/3, /*splat_count=*/1);

    // GAUSSIAN_DATA chunk: CHUNK_FLAG_COMPRESSED (bit 0) | ZSTD (bits 8+). Only 8
    // compressed bytes claim ~4 GiB decompressed -- a hostile ratio far beyond any
    // real codec, so the ratio guard trips before allocation.
    const uint32_t compressed_flags = 0x1u | (uint32_t(GaussianSplatting::CompressionType::ZSTD) << 8);
    const uint32_t bogus_original_size = 0xFFFFFFFFu; // ~4 GiB from 8 compressed bytes
    PackedByteArray fake_compressed;
    fake_compressed.resize(8); // arbitrary; the ratio guard trips before decompression runs
    const uint32_t gaussian_payload_size = uint32_t(sizeof(uint32_t)) + uint32_t(fake_compressed.size());
    file->store_32((uint32_t)GaussianSplatting::ChunkType::GAUSSIAN_DATA);
    file->store_32(gaussian_payload_size);
    file->store_32(0); // checksum (validation disabled below)
    file->store_32(compressed_flags);
    file->store_32(bogus_original_size);
    file->store_buffer(fake_compressed);

    // END_OF_FILE chunk.
    file->store_32((uint32_t)GaussianSplatting::ChunkType::END_OF_FILE);
    file->store_32(0);
    file->store_32(0);
    file->store_32(0);
    file.unref();

    GaussianSplatting::GaussianSceneSerializer serializer;
    serializer.set_enable_checksum(false); // isolate the size cap from checksum verification
    Ref<GaussianData> loaded_data;
    loaded_data.instantiate();
    Error load_err = serializer.load_scene(path, loaded_data.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT,
            "load_scene must reject a decompress-bomb chunk (hostile decompressed:compressed ratio)");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] Decompressed-size guard: codec-physical bound + corroborated ceiling") {
    // Regression (PR #618 independent review round 2). The guard must satisfy BOTH
    // of these at once, which a single tunable ratio provably cannot:
    //   (c) it must never reject a stream our OWN writer can produce, and
    //   (d) a small compressed payload must not buy a huge allocation for free.
    //
    // It is therefore no longer a ratio heuristic. It is the conjunction of the
    // uint32 field width, the codec's PHYSICAL maximum expansion, and a ceiling
    // corroborated by an independent (checksum-covered) field elsewhere in the file.
    //
    // The guard is tested directly at the boundary (rather than by allocating a
    // multi-GiB buffer end to end), because it runs BEFORE the resize().
    using Serializer = GaussianSplatting::GaussianSceneSerializer;
    const GaussianSplatting::CompressionType ZSTD = GaussianSplatting::CompressionType::ZSTD;
    const GaussianSplatting::CompressionType LZ4 = GaussianSplatting::CompressionType::LZ4;
    const uint64_t UNBOUNDED = uint64_t(UINT32_MAX); // corroboration wide open; isolate the other bounds
    const uint64_t NO_BUDGET = UINT64_MAX; // memory budget wide open; the budget axis is tested separately

    // -- No absolute INT32_MAX cap. A legitimate large asset (~15M splats at 144 B
    // each -> a > INT32_MAX uncompressed Zstd chunk) whose compressed size is
    // proportionate MUST still be accepted; this fork's Compression::decompress
    // takes an int64 destination size and MODE_ZSTD has no INT32_MAX guard.
    const uint64_t big_original = uint64_t(INT32_MAX) + (64ull * 1024 * 1024); // ~2.06 GiB
    CHECK_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(big_original, big_original / 2, ZSTD, UNBOUNDED, NO_BUDGET),
            "A > INT32_MAX chunk with a proportionate compressed size must be accepted (no absolute INT32_MAX cap)");
    CHECK_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(uint64_t(UINT32_MAX), uint64_t(UINT32_MAX) / 2, ZSTD, UNBOUNDED, NO_BUDGET),
            "A chunk at the uint32 ceiling with a proportionate compressed size must be accepted");

    // -- (c) The codec-physical bound must not reject real, measured writer output.
    // Zstd level 3 on 1,000,000 identical 144-byte splats produces 13,347 bytes
    // from a 144,000,004-byte payload (ratio ~10,789:1). The OLD 4096:1 ratio bound
    // rejected exactly this. Measured values, not estimates.
    CHECK_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(144000004ull, 13347ull, ZSTD, UNBOUNDED, NO_BUDGET),
            "Measured real writer output (1M identical splats, ratio ~10,789:1) must be accepted");
    // And at Zstd's own structural limit: all-zero input tops out near 32,768:1
    // (measured 268,435,456 B -> 8,211 B). The bound must still clear that.
    CHECK_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(268435456ull, 8211ull, ZSTD, UNBOUNDED, NO_BUDGET),
            "Zstd at its structural max expansion (~32,696:1, measured) must be accepted");

    // -- The codec-physical bound is still a real bound: nothing may exceed it.
    // Zstd cannot emit more than ~32768 bytes per compressed byte, ever.
    CHECK_FALSE_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(uint64_t(UINT32_MAX), 8, ZSTD, UNBOUNDED, NO_BUDGET),
            "A few-byte chunk claiming ~4 GiB decompressed exceeds Zstd's physical expansion limit");
    // FastLZ expands far less than Zstd, and the bound is codec-aware.
    CHECK_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(64ull * 1024, 1024, LZ4, UNBOUNDED, NO_BUDGET),
            "FastLZ within its allowance must be accepted");
    CHECK_FALSE_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(64ull * 1024 * 1024, 1024, LZ4, UNBOUNDED, NO_BUDGET),
            "FastLZ cannot expand 1 KiB to 64 MiB; the bound must be codec-aware, not one global ratio");
    // -- FastLZ LEVEL 2 (thirdparty/misc/fastlz.c:566-571 selects it for any input
    // >= 64 KiB, which is every scene worth compressing) has NO match-length cap.
    // It spends one 0xFF byte per 255 output bytes (fastlz.c:444-447, decoded at
    // :528-535), so it reaches ~255:1 -- not the 88:1/128:1 of level 1's 264-byte
    // MAX_LEN. A bound of 128 REJECTED this writer's own LZ4 output; these two
    // cases pin the corrected bound from both sides and fail if it regresses.
    CHECK_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(200ull * 1024 * 1024, 1024ull * 1024, LZ4, UNBOUNDED, NO_BUDGET),
            "FastLZ level 2 reaches ~255:1; a 200:1 claim is physically producible and must be accepted (a 128:1 bound rejects it)");
    CHECK_FALSE_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(300ull * 1024 * 1024, 1024ull * 1024, LZ4, UNBOUNDED, NO_BUDGET),
            "FastLZ still cannot exceed ~255:1; a 300:1 claim must be rejected");

    // -- Field-width bound.
    CHECK_FALSE_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(uint64_t(UINT32_MAX) + 1, uint64_t(UINT32_MAX), ZSTD, UNBOUNDED, NO_BUDGET),
            "A declared size beyond the uint32 on-disk field width must be rejected");
    // -- No input bytes can produce output bytes.
    CHECK_FALSE_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(1024, 0, ZSTD, UNBOUNDED, NO_BUDGET),
            "A zero-byte compressed payload cannot decompress to anything");

    // -- (d) The corroborated ceiling is what stops cheap amplification. A payload
    // that clears the codec bound is STILL refused when nothing in the file
    // justifies an allocation that large.
    CHECK_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(16ull * 1024 * 1024, 4096, ZSTD, 16ull * 1024 * 1024, NO_BUDGET),
            "A claim exactly at the corroborated ceiling must be accepted");
    CHECK_FALSE_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(16ull * 1024 * 1024 + 1, 4096, ZSTD, 16ull * 1024 * 1024, NO_BUDGET),
            "One byte past the corroborated ceiling must be rejected");
    // The concrete reviewer scenario: ~1 MiB of compressed bytes must not buy ~4 GiB
    // when the surrounding file corroborates only a small scene. (1 MiB clears the
    // Zstd physical bound on its own -- 32768:1 -> 32 GiB -- so corroboration, not
    // the ratio, is what refuses it.)
    const uint64_t one_mib_compressed = 1024ull * 1024;
    CHECK_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(uint64_t(UINT32_MAX), one_mib_compressed, ZSTD, UNBOUNDED, NO_BUDGET),
            "Precondition: ~1 MiB of Zstd input CAN physically emit ~4 GiB, so the codec bound alone cannot refuse it");
    const uint64_t small_scene_corroborated = uint64_t(sizeof(uint32_t)) + 1000ull * uint64_t(sizeof(Gaussian));
    CHECK_FALSE_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(uint64_t(UINT32_MAX), one_mib_compressed, ZSTD, small_scene_corroborated, NO_BUDGET),
            "~1 MiB compressed must NOT buy ~4 GiB when the scene header corroborates only 1000 splats");

    // -- The uncorroborated ceiling applied to ANIMATION_DATA is a hard absolute cap.
    CHECK_FALSE_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(
                                GaussianSplatting::GSF_MAX_UNCORROBORATED_DECOMPRESSED_CHUNK_SIZE + 1,
                                one_mib_compressed, ZSTD,
                                GaussianSplatting::GSF_MAX_UNCORROBORATED_DECOMPRESSED_CHUNK_SIZE, NO_BUDGET),
            "An uncorroborated chunk may not exceed the absolute uncorroborated ceiling");

    // -- (e) The memory budget: the ONLY bound that refuses an internally
    // CONSISTENT hostile file. Here the claim clears the uint32 width, the codec
    // bound (1 MiB of Zstd can physically emit 32 GiB) AND corroboration (the
    // header declares enough splats), exactly as a self-consistent attacker would
    // arrange -- so only the budget can refuse it.
    // The largest self-consistent GAUSSIAN_DATA claim the format permits: the
    // chunk payload is a uint32 field, so at 144 B/splat the ceiling is
    // 29,826,161 splats => 4 + 29,826,161*144 = 4,294,967,188 bytes (just under
    // UINT32_MAX = 4,294,967,295). A hostile file sets its header splat_count to
    // exactly this, so the claim corroborates itself.
    const uint64_t max_self_consistent_claim = uint64_t(sizeof(uint32_t)) + 29826161ull * uint64_t(sizeof(Gaussian));
    CHECK_MESSAGE(max_self_consistent_claim == 4294967188ull,
            "The documented worst-case self-consistent claim must match the format's actual arithmetic");
    CHECK_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(max_self_consistent_claim, one_mib_compressed, ZSTD, max_self_consistent_claim, NO_BUDGET),
            "Precondition: a self-consistent ~4 GiB claim passes every file-derived bound, so the budget is the only remaining defense");
    CHECK_FALSE_MESSAGE(Serializer::is_decompressed_chunk_size_plausible(max_self_consistent_claim, one_mib_compressed, ZSTD, max_self_consistent_claim, 1024ull * 1024 * 1024),
            "A self-consistent ~4 GiB claim must be refused by a 1 GiB memory budget");
    CHECK_MESSAGE(GaussianSplatting::GSF_MIN_LOAD_ALLOCATION_BUDGET_BYTES >= 64ull * 1024 * 1024,
            "The budget floor must stay large enough that ordinary scenes load on a machine that cannot report its memory");
}

TEST_CASE("[GaussianSplatting][Persistence] Reader loads its OWN writer's highly compressible scene (round-trip)") {
    // THE regression for PR #618 review finding (c): the decompression guard was a
    // 4096:1 ratio bound with a 16 MiB floor, but Zstd reaches ~10,800:1 on a
    // legitimately repetitive scene. Every scene above the floor with strongly
    // repeated splat data therefore SAVED fine and then FAILED to load with
    // ERR_FILE_CORRUPT -- the reader rejecting its own writer's output, i.e. silent
    // data loss on a user's saved scene.
    //
    // This is a genuine end-to-end round-trip: save with this writer, load with
    // this reader, compare. It fails on the pre-fix guard.
    //
    // 150,000 identical splats -> a 21,600,004-byte payload, comfortably past the
    // old 16 MiB floor, compressing to ~2 KB (ratio ~10,098:1 measured).
    const int kSplats = 150000;

    Ref<GaussianData> data;
    data.instantiate();
    {
        Vector<Gaussian> gaussians;
        gaussians.resize(kSplats);
        Gaussian g = {};
        g.position = Vector3(1.0f, 2.0f, 3.0f);
        g.scale = Vector3(0.5f, 0.5f, 0.5f);
        g.rotation = Quaternion();
        g.opacity = 0.75f;
        g.sh_dc = Color(0.25f, 0.5f, 0.75f, 1.0f);
        for (int i = 0; i < kSplats; i++) {
            gaussians.write[i] = g; // Identical => maximally compressible, and legitimate.
        }
        data->set_gaussians(gaussians);
    }
    CHECK_MESSAGE(data->get_count() == kSplats, "Fixture scene should hold the requested splat count");

    const String path = _make_persistence_fixture_path("test_highly_compressible_roundtrip");
    CHECK_MESSAGE(_ensure_persistence_fixture_dir(path), "Persistence fixture directory should be available");

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_compression_type(GaussianSplatting::CompressionType::ZSTD);
    const Error save_err = writer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "Writer must save a highly compressible scene");

    // Prove the fixture really is in the regressed regime: the payload must exceed
    // the old 16 MiB floor AND compress far past the old 4096:1 ratio. Otherwise
    // this test would pass vacuously even with the buggy guard restored.
    {
        Ref<FileAccess> probe = FileAccess::open(path, FileAccess::READ);
        CHECK_MESSAGE(probe.is_valid(), "Saved fixture should be readable");
        if (probe.is_valid()) {
            const uint64_t on_disk = probe->get_length();
            const uint64_t uncompressed_payload = uint64_t(sizeof(uint32_t)) + uint64_t(kSplats) * uint64_t(sizeof(Gaussian));
            CHECK_MESSAGE(uncompressed_payload > 16ull * 1024 * 1024,
                    "Fixture payload must exceed the old 16 MiB floor for this to be a real regression test");
            CHECK_MESSAGE(on_disk * 4096 < uncompressed_payload,
                    "Fixture must compress past the old 4096:1 ratio bound (else the test is vacuous)");
        }
    }

    // The actual claim: this reader loads what this writer wrote.
    GaussianSplatting::GaussianSceneSerializer reader;
    Ref<GaussianData> loaded;
    loaded.instantiate();
    const Error load_err = reader.load_scene(path, loaded.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == OK,
            "Reader MUST load its own writer's highly compressible scene (pre-fix: ERR_FILE_CORRUPT)");
    CHECK_MESSAGE(loaded->get_count() == kSplats, "Round-tripped scene must keep every splat");
    if (loaded->get_count() == kSplats) {
        const LocalVector<Gaussian> &out = loaded->get_gaussian_storage();
        CHECK_MESSAGE(out[0].position.is_equal_approx(Vector3(1.0f, 2.0f, 3.0f)), "First splat position must survive");
        CHECK_MESSAGE(out[kSplats - 1].position.is_equal_approx(Vector3(1.0f, 2.0f, 3.0f)), "Last splat position must survive");
        CHECK_MESSAGE(Math::is_equal_approx(out[kSplats - 1].opacity, 0.75f), "Last splat opacity must survive");
    }

    // validate_file() must agree with load_scene() on the very same file.
    CHECK_MESSAGE(reader.validate_file(path) == OK,
            "validate_file must accept a file load_scene accepts");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] Reader loads its OWN writer's LZ4/FastLZ scene (round-trip)") {
    // BLOCKER 1 from the PR #618 independent review. The Zstd side of this bug was
    // fixed; the FastLZ side was not. CompressionType::LZ4 maps to Godot's
    // MODE_FASTLZ, which calls fastlz_compress() on the whole buffer and therefore
    // selects fastlz LEVEL 2 for any input >= 64 KiB. Level 2 has no match-length
    // cap and spends one 0xFF byte per 255 output bytes, so it reaches ~255:1 --
    // but the guard capped FastLZ expansion at 128:1, describing level 1's
    // 264-byte MAX_LEN instead. A repetitive scene saved with LZ4 therefore SAVED
    // fine and then failed to load: the reader rejecting its own writer's output.
    //
    // The pre-existing tests missed this because none of them ever ran an LZ4
    // scene end to end -- they only probed the bound at 64:1 (accepted) and at an
    // absurd ratio (rejected), i.e. on both sides of the real 255:1 boundary but
    // never across it.
    const int kSplats = 150000;

    Ref<GaussianData> data;
    data.instantiate();
    {
        Vector<Gaussian> gaussians;
        gaussians.resize(kSplats);
        Gaussian g = {};
        g.position = Vector3(4.0f, 5.0f, 6.0f);
        g.scale = Vector3(0.25f, 0.25f, 0.25f);
        g.rotation = Quaternion();
        g.opacity = 0.5f;
        g.sh_dc = Color(0.1f, 0.2f, 0.3f, 1.0f);
        for (int i = 0; i < kSplats; i++) {
            gaussians.write[i] = g; // Identical => maximally compressible, and legitimate.
        }
        data->set_gaussians(gaussians);
    }
    CHECK_MESSAGE(data->get_count() == kSplats, "Fixture scene should hold the requested splat count");

    const String path = _make_persistence_fixture_path("test_lz4_roundtrip");
    CHECK_MESSAGE(_ensure_persistence_fixture_dir(path), "Persistence fixture directory should be available");

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_compression_type(GaussianSplatting::CompressionType::LZ4);
    const Error save_err = writer.save_scene(path, data.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "Writer must save a highly compressible scene with LZ4/FastLZ");

    // Prove the fixture is genuinely past the old 128:1 bound, otherwise this test
    // would pass vacuously even with the buggy bound restored.
    {
        Ref<FileAccess> probe = FileAccess::open(path, FileAccess::READ);
        CHECK_MESSAGE(probe.is_valid(), "Saved LZ4 fixture should be readable");
        if (probe.is_valid()) {
            const uint64_t on_disk = probe->get_length();
            const uint64_t uncompressed_payload = uint64_t(sizeof(uint32_t)) + uint64_t(kSplats) * uint64_t(sizeof(Gaussian));
            CHECK_MESSAGE(uncompressed_payload > 64ull * 1024,
                    "Fixture payload must exceed 64 KiB so FastLZ level 2 (the uncapped one) is selected");
            CHECK_MESSAGE(on_disk * 128 < uncompressed_payload,
                    "Fixture must compress past the old 128:1 FastLZ bound (else the test is vacuous)");
        }
    }

    // The actual claim: this reader loads what this writer wrote.
    GaussianSplatting::GaussianSceneSerializer reader;
    Ref<GaussianData> loaded;
    loaded.instantiate();
    const Error load_err = reader.load_scene(path, loaded.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == OK,
            "Reader MUST load its own writer's LZ4 scene (pre-fix: ERR_FILE_CORRUPT at the 128:1 bound)");
    CHECK_MESSAGE(loaded->get_count() == kSplats, "Round-tripped LZ4 scene must keep every splat");
    if (loaded->get_count() == kSplats) {
        const LocalVector<Gaussian> &out = loaded->get_gaussian_storage();
        CHECK_MESSAGE(out[0].position.is_equal_approx(Vector3(4.0f, 5.0f, 6.0f)), "First splat position must survive LZ4");
        CHECK_MESSAGE(out[kSplats - 1].position.is_equal_approx(Vector3(4.0f, 5.0f, 6.0f)), "Last splat position must survive LZ4");
        CHECK_MESSAGE(Math::is_equal_approx(out[kSplats - 1].opacity, 0.5f), "Last splat opacity must survive LZ4");
    }

    CHECK_MESSAGE(reader.validate_file(path) == OK,
            "validate_file must accept an LZ4 file load_scene accepts");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] Loading a zero-splat scene CLEARS the target") {
    // BLOCKER 2 from the PR #618 independent review. The writer omits the
    // GAUSSIAN_DATA chunk when get_count() == 0, and the reader used to commit
    // gaussians only when that chunk existed. So saving an empty scene and loading
    // it into a populated target returned OK while leaving the PREVIOUS scene's
    // splats in place -- the load reported success and the target held data that
    // was not what was saved. Silent data corruption from an ordinary user action.
    const String path = _make_persistence_fixture_path("test_empty_scene_clears_target");
    CHECK_MESSAGE(_ensure_persistence_fixture_dir(path), "Persistence fixture directory should be available");

    // Save a deliberately EMPTY scene.
    Ref<GaussianData> empty;
    empty.instantiate();
    CHECK_MESSAGE(empty->get_count() == 0, "Fixture source scene must be empty");

    GaussianSplatting::GaussianSceneSerializer writer;
    const Error save_err = writer.save_scene(path, empty.ptr(), nullptr, Dictionary());
    CHECK_MESSAGE(save_err == OK, "Writer must save a zero-splat scene");

    // Load it into a target that ALREADY holds a different scene.
    Ref<GaussianData> target;
    target.instantiate();
    {
        Vector<Gaussian> stale;
        stale.resize(64);
        for (int i = 0; i < 64; i++) {
            stale.write[i].position = Vector3(i, i, i);
            stale.write[i].scale = Vector3(1, 1, 1);
            stale.write[i].rotation = Quaternion();
            stale.write[i].opacity = 1.0f;
        }
        target->set_gaussians(stale);
    }
    CHECK_MESSAGE(target->get_count() == 64, "Target must start populated, or the test proves nothing");

    GaussianSplatting::GaussianSceneSerializer reader;
    const Error load_err = reader.load_scene(path, target.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == OK, "Loading a zero-splat scene must succeed");
    CHECK_MESSAGE(target->get_count() == 0,
            "Loading a zero-splat scene MUST clear the target (pre-fix: the previous scene's 64 splats survived a successful load)");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] A checksum-disabled loader rejects a tampered protected file") {
    // MAJOR 3 from the PR #618 independent review. validate_file() always ran a
    // strict checksum probe and rejected a tampered file that advertised checksum
    // protection, but load_scene() parsed with checksums disabled whenever
    // enable_checksum == false. A checksum-disabled loader would therefore happily
    // LOAD a tampered protected file that validate_file() on the same instance
    // rejected -- the two disagreed about the same bytes.
    //
    // enable_checksum is a WRITER setting; on read it may add verification but
    // never remove verification the FILE asked for. Both paths now share one
    // policy, so both must reject, and they must return the SAME verdict.
    const String path = _make_persistence_fixture_path("test_tampered_protected_checksum_off");
    CHECK_MESSAGE(_ensure_persistence_fixture_dir(path), "Persistence fixture directory should be available");

    Ref<GaussianData> data;
    data.instantiate();
    {
        Vector<Gaussian> gaussians;
        gaussians.resize(8);
        for (int i = 0; i < 8; i++) {
            gaussians.write[i].position = Vector3(i, 0, 0);
            gaussians.write[i].scale = Vector3(1, 1, 1);
            gaussians.write[i].rotation = Quaternion();
            gaussians.write[i].opacity = 1.0f;
        }
        data->set_gaussians(gaussians);
    }

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_enable_checksum(true); // The file ADVERTISES checksum protection.
    CHECK_MESSAGE(writer.save_scene(path, data.ptr(), nullptr, Dictionary()) == OK,
            "Checksum-protected fixture should save");

    // Tamper a header-payload byte that is not the magic, so the ONLY thing that
    // can catch it is the checksum.
    {
        Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::READ_WRITE);
        CHECK_MESSAGE(file.is_valid(), "Should be able to mutate the protected fixture");
        if (file.is_valid()) {
            const uint64_t creation_time_offset = uint64_t(GaussianSplatting::GSF_CHUNK_HEADER_SIZE) + 40;
            file->seek(creation_time_offset);
            const uint8_t original = file->get_8();
            file->seek(creation_time_offset);
            file->store_8(original ^ 0xFF);
        }
    }

    // A loader with checksums explicitly DISABLED.
    GaussianSplatting::GaussianSceneSerializer lenient;
    lenient.set_enable_checksum(false);

    Ref<GaussianData> loaded;
    loaded.instantiate();
    const Error load_err = lenient.load_scene(path, loaded.ptr(), nullptr, nullptr);
    const Error validate_err = lenient.validate_file(path);

    CHECK_MESSAGE(load_err != OK,
            "A checksum-disabled load MUST still reject a tampered file that advertises checksum protection (pre-fix: returned OK)");
    CHECK_MESSAGE(validate_err != OK,
            "validate_file must reject the same tampered protected file");
    CHECK_MESSAGE(load_err == validate_err,
            "load_scene and validate_file must return the SAME verdict on the same file -- that is the whole point of MAJOR 3");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence] A checksum-disabled loader still loads an UNPROTECTED file") {
    // Guard the other side of MAJOR 3: unifying the checksum policy must not turn
    // into "always verify", which would break files legitimately written without
    // checksums. Nothing in such a file claims protection, so verification is
    // correctly skipped and both paths accept it.
    const String path = _make_persistence_fixture_path("test_unprotected_checksum_off");
    CHECK_MESSAGE(_ensure_persistence_fixture_dir(path), "Persistence fixture directory should be available");

    Ref<GaussianData> data;
    data.instantiate();
    {
        Vector<Gaussian> gaussians;
        gaussians.resize(8);
        for (int i = 0; i < 8; i++) {
            gaussians.write[i].position = Vector3(i, 1, 2);
            gaussians.write[i].scale = Vector3(1, 1, 1);
            gaussians.write[i].rotation = Quaternion();
            gaussians.write[i].opacity = 1.0f;
        }
        data->set_gaussians(gaussians);
    }

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_enable_checksum(false);
    CHECK_MESSAGE(writer.save_scene(path, data.ptr(), nullptr, Dictionary()) == OK,
            "Unprotected fixture should save");

    GaussianSplatting::GaussianSceneSerializer lenient;
    lenient.set_enable_checksum(false);
    Ref<GaussianData> loaded;
    loaded.instantiate();
    CHECK_MESSAGE(lenient.load_scene(path, loaded.ptr(), nullptr, nullptr) == OK,
            "A checksum-disabled loader must still load a file written without checksums");
    CHECK_MESSAGE(loaded->get_count() == 8, "Unprotected round-trip must keep every splat");
    CHECK_MESSAGE(lenient.validate_file(path) == OK,
            "validate_file must agree that an unprotected file is loadable");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] The load memory budget bounds a real load") {
    // MAJOR 5 from the PR #618 independent review. The codec and corroboration
    // bounds check a chunk against the FILE, so an internally CONSISTENT hostile
    // file satisfies both and still forces a multi-GiB allocation. The absolute
    // budget is the only remaining defense. This test proves the budget is
    // actually WIRED INTO the load path rather than merely existing as a pure
    // function -- pinning it below the fixture's size must make a real,
    // well-formed, otherwise-valid load fail.
    const String path = _make_persistence_fixture_path("test_load_memory_budget");
    CHECK_MESSAGE(_ensure_persistence_fixture_dir(path), "Persistence fixture directory should be available");

    const int kSplats = 4096; // ~590 KB uncompressed payload.
    Ref<GaussianData> data;
    data.instantiate();
    {
        Vector<Gaussian> gaussians;
        gaussians.resize(kSplats);
        for (int i = 0; i < kSplats; i++) {
            gaussians.write[i].position = Vector3(i % 97, i % 31, i % 13);
            gaussians.write[i].scale = Vector3(1, 1, 1);
            gaussians.write[i].rotation = Quaternion();
            gaussians.write[i].opacity = 1.0f;
        }
        data->set_gaussians(gaussians);
    }

    GaussianSplatting::GaussianSceneSerializer writer;
    CHECK_MESSAGE(writer.save_scene(path, data.ptr(), nullptr, Dictionary()) == OK,
            "Budget fixture should save");

    GaussianSplatting::GaussianSceneSerializer reader;

    // With the budget pinned far below the payload, the load must be refused.
    GaussianSplatting::GaussianSceneSerializer::set_load_allocation_budget_override(4096);
    Ref<GaussianData> refused;
    refused.instantiate();
    const Error budgeted_err = reader.load_scene(path, refused.ptr(), nullptr, nullptr);
    GaussianSplatting::GaussianSceneSerializer::set_load_allocation_budget_override(0);

    CHECK_MESSAGE(budgeted_err != OK,
            "A load whose chunk exceeds the pinned memory budget must be refused (proves the budget is wired into the load path)");
    CHECK_MESSAGE(refused->get_count() == 0,
            "A budget-refused load must not commit anything to the target");

    // And with the budget restored to its OS-derived default, the very same file
    // must load -- the budget must not reject ordinary scenes.
    Ref<GaussianData> loaded;
    loaded.instantiate();
    CHECK_MESSAGE(reader.load_scene(path, loaded.ptr(), nullptr, nullptr) == OK,
            "The same file must load once the budget is back to its OS-derived default");
    CHECK_MESSAGE(loaded->get_count() == kSplats, "The unbudgeted load must keep every splat");
    CHECK_MESSAGE(GaussianSplatting::GaussianSceneSerializer::get_load_allocation_budget_bytes()
                    >= GaussianSplatting::GSF_MIN_LOAD_ALLOCATION_BUDGET_BYTES,
            "The default budget must never fall below the documented floor");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] GAUSSIAN_DATA count must match the scene header exactly") {
    // MAJOR 4 from the PR #618 independent review: the GAUS payload schema was
    // checked with `expected > actual`, i.e. as a LOWER bound. Trailing decoded
    // bytes were accepted, and a chunk count SMALLER than the header's splat_count
    // was accepted -- so a file could disagree with itself about how many splats
    // it holds and still load "successfully", silently yielding a different scene
    // than the one that was saved. The schema is exact and is now enforced as such.
    const String path = _make_persistence_fixture_path("test_gaus_count_mismatch");
    CHECK_MESSAGE(_ensure_persistence_fixture_dir(path), "Persistence fixture directory should be available");

    const int kSplats = 32;
    Ref<GaussianData> data;
    data.instantiate();
    {
        Vector<Gaussian> gaussians;
        gaussians.resize(kSplats);
        for (int i = 0; i < kSplats; i++) {
            gaussians.write[i].position = Vector3(i, 0, 0);
            gaussians.write[i].scale = Vector3(1, 1, 1);
            gaussians.write[i].rotation = Quaternion();
            gaussians.write[i].opacity = 1.0f;
        }
        data->set_gaussians(gaussians);
    }

    GaussianSplatting::GaussianSceneSerializer writer;
    writer.set_compression_type(GaussianSplatting::CompressionType::NONE);
    writer.set_enable_checksum(false); // So the tamper is caught by the schema, not the checksum.
    CHECK_MESSAGE(writer.save_scene(path, data.ptr(), nullptr, Dictionary()) == OK,
            "Uncompressed, unchecksummed fixture should save");

    // Rewrite the GAUSSIAN_DATA chunk's leading splat count to fewer splats than
    // the payload (and than the scene header) actually carries.
    bool patched = false;
    {
        Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::READ_WRITE);
        CHECK_MESSAGE(file.is_valid(), "Should be able to mutate the fixture");
        if (file.is_valid()) {
            const uint64_t file_length = file->get_length();
            file->seek(0);
            while (file->get_position() + uint64_t(GaussianSplatting::GSF_CHUNK_HEADER_SIZE) <= file_length) {
                const uint32_t chunk_type = file->get_32();
                const uint32_t chunk_size = file->get_32();
                file->get_32(); // checksum
                file->get_32(); // flags
                const uint64_t payload_offset = file->get_position();
                if (payload_offset > file_length || uint64_t(chunk_size) > file_length - payload_offset) {
                    break;
                }
                if (chunk_type == uint32_t(GaussianSplatting::ChunkType::GAUSSIAN_DATA)) {
                    file->seek(payload_offset);
                    file->store_32(uint32_t(kSplats - 1)); // One splat short of the truth.
                    patched = true;
                    break;
                }
                file->seek(payload_offset + uint64_t(chunk_size));
            }
        }
    }
    CHECK_MESSAGE(patched, "Fixture should contain a GAUSSIAN_DATA chunk to patch");

    GaussianSplatting::GaussianSceneSerializer reader;
    reader.set_enable_checksum(false);
    Ref<GaussianData> loaded;
    loaded.instantiate();
    const Error load_err = reader.load_scene(path, loaded.ptr(), nullptr, nullptr);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT,
            "A GAUSSIAN_DATA count that disagrees with the payload length and the scene header must be rejected (pre-fix: loaded 31 of 32 splats and returned OK)");
    CHECK_MESSAGE(loaded->get_count() == 0, "A rejected load must not commit anything");
    CHECK_MESSAGE(reader.validate_file(path) == load_err,
            "validate_file must return the same verdict as load_scene");

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Truncated 16-byte chunk header is rejected") {
    // PR #618 review finding (a). FileAccess::get_32() zero-fills bytes it cannot
    // read and reports no error, so a file ending mid-chunk-header used to produce
    // a fully-formed-looking header of trailing zeros. Truncating the file inside
    // its FINAL END_OF_FILE header is the nastiest case: the reader still saw a
    // terminator and still counted the declared number of chunks, so a truncated
    // file was accepted as complete.
    for (int drop_bytes = 1; drop_bytes <= 15; drop_bytes++) {
        const String path = _make_persistence_fixture_path("test_partial_eof_header_" + itos(drop_bytes));
        if (!_ensure_persistence_fixture_dir(path)) {
            continue;
        }

        Ref<GaussianData> data;
        data.instantiate();
        {
            Vector<Gaussian> gaussians;
            gaussians.resize(2);
            for (int i = 0; i < 2; i++) {
                gaussians.write[i].position = Vector3(i, 0, 0);
                gaussians.write[i].scale = Vector3(1, 1, 1);
                gaussians.write[i].rotation = Quaternion();
                gaussians.write[i].opacity = 1.0f;
            }
            data->set_gaussians(gaussians);
        }

        GaussianSplatting::GaussianSceneSerializer serializer;
        const Error save_err = serializer.save_scene(path, data.ptr(), nullptr, Dictionary());
        CHECK_MESSAGE(save_err == OK, "Baseline fixture should save");

        // Lop off part (never all) of the trailing 16-byte END_OF_FILE header.
        CHECK_MESSAGE(_truncate_fixture_tail(path, uint64_t(drop_bytes)),
                "Should be able to truncate the fixture tail");

        Ref<GaussianData> loaded;
        loaded.instantiate();
        const Error load_err = serializer.load_scene(path, loaded.ptr(), nullptr, nullptr);
        CHECK_MESSAGE(load_err != OK,
                vformat("load_scene must reject a file truncated %d byte(s) into its final chunk header", drop_bytes));
        CHECK_MESSAGE(loaded->get_count() == 0,
                "A rejected load must leave the target untouched (transactional)");

        // validate_file must agree -- it must never accept what load_scene rejects.
        CHECK_MESSAGE(serializer.validate_file(path) != OK,
                vformat("validate_file must reject a file truncated %d byte(s) into its final chunk header", drop_bytes));

        _remove_persistence_fixture(path);
    }
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] validate_file and load_scene agree about compression") {
    // PR #618 review finding (b). The chunk readers used to fall back to the
    // *serializer instance's* compression_type when a compressed chunk's codec bits
    // were zero. validate_file() runs a default-constructed probe (Zstd) while
    // load_scene() runs on the caller's instance, so the same bytes were decoded
    // under two different rules: a file could validate OK and then fail (or worse,
    // mis-decode) on load. The codec is now resolved purely from the chunk's own
    // on-disk flags, and an unknown/zero codec id fails closed on BOTH paths.
    const String path = _make_persistence_fixture_path("test_codec_bits_zero");
    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to create codec-mismatch fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    _write_gsf_header_chunk(file, /*total_chunks=*/3, /*splat_count=*/1);

    // GAUSSIAN_DATA flagged COMPRESSED but with codec id 0 -- the ambiguity that
    // used to be resolved from instance state.
    const uint32_t compressed_flag_no_codec = 0x1u; // bits 8..15 (codec id) deliberately zero
    PackedByteArray body;
    body.resize(16);
    for (int i = 0; i < body.size(); i++) {
        body.write[i] = uint8_t(i);
    }
    file->store_32((uint32_t)GaussianSplatting::ChunkType::GAUSSIAN_DATA);
    file->store_32(uint32_t(sizeof(uint32_t)) + uint32_t(body.size()));
    file->store_32(0); // checksum (checksums disabled on the readers below)
    file->store_32(compressed_flag_no_codec);
    file->store_32(uint32_t(sizeof(uint32_t)) + uint32_t(sizeof(Gaussian))); // declared decompressed size
    file->store_buffer(body);

    file->store_32((uint32_t)GaussianSplatting::ChunkType::END_OF_FILE);
    file->store_32(0);
    file->store_32(0);
    file->store_32(0);
    file.unref();

    // Every instance compression_type setting must reach the SAME verdict, and it
    // must be the same verdict validate_file reaches. Before the fix, the NONE and
    // LZ4 instances diverged from the Zstd validation probe.
    const GaussianSplatting::CompressionType settings[] = {
        GaussianSplatting::CompressionType::NONE,
        GaussianSplatting::CompressionType::ZSTD,
        GaussianSplatting::CompressionType::LZ4,
    };
    for (const GaussianSplatting::CompressionType setting : settings) {
        GaussianSplatting::GaussianSceneSerializer serializer;
        serializer.set_enable_checksum(false);
        serializer.set_compression_type(setting);

        Ref<GaussianData> loaded;
        loaded.instantiate();
        const Error load_err = serializer.load_scene(path, loaded.ptr(), nullptr, nullptr);
        const Error validate_err = serializer.validate_file(path);

        CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT,
                vformat("A compressed chunk with codec id 0 must fail closed on load regardless of instance compression_type (%d)", (int)setting));
        CHECK_MESSAGE(validate_err != OK,
                vformat("validate_file must reject what load_scene rejects (instance compression_type %d)", (int)setting));
        CHECK_MESSAGE((load_err == OK) == (validate_err == OK),
                vformat("validate_file and load_scene must agree about compression (instance compression_type %d)", (int)setting));
    }

    _remove_persistence_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] Incremental loader rejects a change payload that fails to decode") {
    // #603b: a change entry whose payload does not decode into a Dictionary must
    // abort the load, NOT be swallowed into a default-valued change. The pre-fix
    // loader decoded via a helper that collapsed a failure to an empty dict and
    // returned OK, so this case fails on base.
    const String path = _make_persistence_fixture_path("test_incremental_bad_decode", ".gsif");
    Ref<FileAccess> file = _open_persistence_fixture(path, FileAccess::WRITE);
    CHECK_MESSAGE(file.is_valid(), "Should be able to create bad-decode incremental fixture");
    if (!file.is_valid()) {
        _remove_persistence_fixture(path);
        return;
    }

    file->store_32(GaussianSplatting::INCREMENTAL_MAGIC);
    file->store_16(GaussianSplatting::INCREMENTAL_VERSION);
    file->store_16(GaussianSplatting::INCREMENTAL_SAVER_LAYOUT_VERSION);
    file->store_64(1); // change timestamp
    file->store_64(0); // baseline timestamp
    file->store_32(0); // baseline splat count
    file->store_32(1); // change_count

    // One structurally valid SPLAT_MODIFIED entry pointing at a 4-byte payload...
    file->store_8((uint8_t)GaussianSplatting::ChangeType::SPLAT_MODIFIED);
    file->store_32(0); // data_offset
    file->store_32(4); // data_size
    file->store_64(1); // timestamp

    // ...but the payload is not a valid encoded Variant (invalid type id), so it
    // cannot decode into a Dictionary.
    file->store_32(0xFFFFFFFFu);
    file.unref();

    GaussianSplatting::GaussianIncrementalSaver saver;
    Error err = saver.load_and_apply_changes(path, nullptr, nullptr);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT,
            "A change payload that fails to decode must be rejected, not silently defaulted");

    _remove_persistence_fixture(path);
}
