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

TEST_CASE("[GaussianSplatting][Persistence] GSF load documents high-order SH loss explicitly") {
    // The GAUSSIAN_DATA chunk format only carries per-splat `Gaussian` struct
    // bytes; it does NOT persist the `sh_high_order_coefficients` sidecar.
    // This test pins that limitation: after a save/load round-trip the loaded
    // GaussianData reports sh_high_order_count == 0 regardless of the source
    // data. If a future format revision starts persisting the sidecar, this
    // test should be updated (and the reconstruction path in
    // _read_gaussian_data_chunk must pass the sidecar to set_gaussian_payload).
    const String path = _make_persistence_fixture_path("test_sh_high_order_loss");
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

    original_data->set_gaussian_payload(gaussians, high_order, 1, high_order_per_splat, false);
    CHECK_MESSAGE(original_data->get_sh_high_order_count() == high_order_per_splat,
            "Source data should carry high-order SH before save");

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
        CHECK_MESSAGE(loaded_data->get_sh_high_order_count() == 0,
                "High-order SH is not carried by the GAUSSIAN_DATA chunk format and must reset to 0 on load");
        CHECK_MESSAGE(loaded_data->get_sh_high_order_coefficients_ptr() == nullptr,
                "High-order SH sidecar must be empty after reconstruction");
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

TEST_CASE("[GaussianSplatting][Persistence] load_scene rejects forward-incompatible future versions") {
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

TEST_CASE("[GaussianSplatting][Persistence] Validation rejects checksum-stripped protected chunked GSF") {
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

TEST_CASE("[GaussianSplatting][Persistence] Validation rejects checksum-tampered chunked GSF") {
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

TEST_CASE("[GaussianSplatting][Persistence] Validation rejects checksum-zeroed protected headers") {
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

TEST_CASE("[GaussianSplatting][Persistence] Validation rejects checksum-zeroed legacy checksummed headers") {
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

TEST_CASE("[GaussianSplatting][Persistence] Validation rejects non-chunked magic-at-byte0 payloads") {
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

TEST_CASE("[GaussianSplatting][Persistence] Validation rejects truncated chunked header payloads") {
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

TEST_CASE("[GaussianSplatting][Persistence] Incremental loader rejects malformed change tables") {
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

TEST_CASE("[GaussianSplatting][Persistence] Incremental loader rejects truncated change table header") {
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

TEST_CASE("[GaussianSplatting][Persistence] Incremental loader rejects out-of-range payload slices") {
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

TEST_CASE("[GaussianSplatting][Persistence] Incremental loader rejects overflow-sized payload slices") {
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
