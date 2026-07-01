#pragma once

#include "test_macros.h"

#include "../core/streaming_chunk_payload_source.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"


namespace {

Gaussian make_gaussian(const Vector3 &p_position, const Color &p_dc) {
    Gaussian g;
    g.position = p_position;
    // Derive scale/rotation/opacity/sh_1 deterministically from position.x so the
    // round-trip can catch a payload path that drops/zeroes these attributes.
    // The signature is unchanged and position + byte-size assertions are unaffected.
    const float k = p_position.x;
    g.scale = Vector3(1.0f + k, 2.0f + 0.5f * k, 3.0f);
    // Non-axis-aligned rotation so x/y/z/w are all non-zero — a zero lane would
    // be hidden by a zero expectation in the component-wise round-trip check.
    g.rotation = Quaternion(Vector3(1.0f, 2.0f, 3.0f).normalized(), 0.1f * (k + 1.0f)).normalized();
    g.opacity = CLAMP(0.05f + 0.03f * k, 0.0f, 1.0f);
    g.sh_dc = p_dc;
    g.sh_1[0] = Vector3(0.1f + 0.05f * k, 0.1f, 0.1f);
    g.sh_1[1] = Vector3();
    g.sh_1[2] = Vector3();
    g.normal = Vector3(0.0f, 1.0f, 0.0f);
    g.area = 1.0f;
    g.brush_axes = Vector2(1.0f, 1.0f);
    g.stroke_age = 0.0f;
    g.painterly_meta = gaussian_pack_painterly_meta(17, 300);
    return g;
}

// Verifies that a round-tripped snapshot gaussian preserves the per-splat
// attributes make_gaussian derives from position.x (scale/rotation/opacity/
// sh_dc/sh_1[0]). Quaternions are compared sign-insensitively (q and -q are the
// same rotation). High-order SH (sh_high) is NOT checked here because these
// fixtures configure sh_high_order = 0 — the snapshot path returns it empty.
void _check_attributes_round_trip(const Gaussian &p_got, const Gaussian &p_expected) {
    CHECK(p_got.scale.is_equal_approx(p_expected.scale));
    // Quaternion equality is sign-insensitive (q and -q encode the same
    // rotation). Compare the RAW stored quaternions COMPONENT-WISE against
    // p_expected.rotation or its negation. (No pre-normalize, which would hide
    // scale corruption; no dot product, which is blind to corruption in
    // components where the expected quaternion is zero.)
    CHECK((p_got.rotation.is_equal_approx(p_expected.rotation) ||
            p_got.rotation.is_equal_approx(-p_expected.rotation)));
    CHECK(Math::abs(p_got.opacity - p_expected.opacity) < 0.02f);
    CHECK(Math::abs(p_got.sh_dc.r - p_expected.sh_dc.r) < 0.02f);
    CHECK(Math::abs(p_got.sh_dc.g - p_expected.sh_dc.g) < 0.02f);
    CHECK(Math::abs(p_got.sh_dc.b - p_expected.sh_dc.b) < 0.02f);
    CHECK(Math::abs(p_got.sh_dc.a - p_expected.sh_dc.a) < 0.02f);
    CHECK(p_got.sh_1[0].is_equal_approx(p_expected.sh_1[0]));
}

Vector<Gaussian> build_gaussians() {
    Vector<Gaussian> gaussians;
    gaussians.resize(4);
    gaussians.write[0] = make_gaussian(Vector3(0.0f, 0.0f, 0.0f), Color(1.0f, 0.0f, 0.0f, 1.0f));
    gaussians.write[1] = make_gaussian(Vector3(1.0f, 0.0f, 0.0f), Color(0.0f, 1.0f, 0.0f, 1.0f));
    gaussians.write[2] = make_gaussian(Vector3(0.0f, 1.0f, 0.0f), Color(0.0f, 0.0f, 1.0f, 1.0f));
    gaussians.write[3] = make_gaussian(Vector3(1.0f, 1.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f));
    return gaussians;
}

Vector<GaussianSplatRenderer::StaticChunk> build_chunks() {
    Vector<GaussianSplatRenderer::StaticChunk> chunks;
    chunks.resize(2);

    GaussianSplatRenderer::StaticChunk first;
    first.bounds = AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.5f, 1.5f, 1.0f));
    first.center = first.bounds.get_center();
    first.radius = 1.5f;
    first.indices.resize(2);
    first.indices.write[0] = 0;
    first.indices.write[1] = 1;
    chunks.write[0] = first;

    GaussianSplatRenderer::StaticChunk second;
    second.bounds = AABB(Vector3(-0.5f, 0.5f, -0.5f), Vector3(1.5f, 1.5f, 1.0f));
    second.center = second.bounds.get_center();
    second.radius = 1.5f;
    second.indices.resize(2);
    second.indices.write[0] = 2;
    second.indices.write[1] = 3;
    chunks.write[1] = second;

    return chunks;
}

struct GsplatWorldSaverGuard {
    Ref<ResourceFormatSaverGaussianSplatWorld> saver;

    GsplatWorldSaverGuard() {
        saver.instantiate();
        ResourceSaver::add_resource_format_saver(saver, true); // true = at_front for priority
    }

    ~GsplatWorldSaverGuard() {
        if (saver.is_valid()) {
            ResourceSaver::remove_resource_format_saver(saver);
        }
	}
};

struct GsplatWorldCompressionSettingGuard {
    StringName key = StringName("rendering/gaussian_splatting/import/gsplatworld_compression_enabled");
    Variant previous_value;

    explicit GsplatWorldCompressionSettingGuard(bool p_enabled) {
        ProjectSettings *ps = ProjectSettings::get_singleton();
        if (!ps) {
            return;
        }
        previous_value = ps->has_setting(key) ? ps->get_setting(key) : Variant(false);
        ps->set_setting(key, p_enabled);
    }

    ~GsplatWorldCompressionSettingGuard() {
        ProjectSettings *ps = ProjectSettings::get_singleton();
        if (ps) {
            ps->set_setting(key, previous_value);
        }
    }
};

String _make_world_io_fixture_path(const String &p_prefix) {
    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String base_temp = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : ".";
    return base_temp.path_join("godotgs_world_io_" + p_prefix + "_" + itos(ticks) + ".gsplatworld");
}

void _remove_world_io_fixture(const String &p_path) {
    DirAccess::remove_absolute(p_path);
}

bool _world_io_file_has_compression_flag(const String &p_path) {
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
    if (!f.is_valid() || f->get_length() < 12) {
        return false;
    }
    f->seek(8); // magic + version
    return (f->get_32() & (1u << 4u)) != 0u;
}

Vector<Gaussian> build_staged_payload_gaussians(uint32_t p_count) {
    Vector<Gaussian> gaussians;
    gaussians.resize(p_count);
    for (uint32_t i = 0; i < p_count; i++) {
        // Distinct, asymmetric, all-NON-ZERO per-splat sh_dc and position so the
        // round-trip asserts catch a DC channel swap/zero AND a dropped position
        // lane (an all-white or zero-lane fixture would hide those, since a zero
        // expectation matches a zeroed result).
        const Color dc(0.1f + float(i % 5) * 0.15f, 0.2f + float(i % 3) * 0.2f,
                0.3f + float(i % 7) * 0.1f, 0.4f + float(i % 2) * 0.25f);
        gaussians.write[i] = make_gaussian(Vector3(float(i) + 1.0f, float(i % 7) + 2.0f, float(i % 3) + 3.0f), dc);
    }
    return gaussians;
}

bool write_staged_payload_fixture(const String &p_path, const Vector<Gaussian> &p_gaussians) {
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
    if (!f.is_valid()) {
        return false;
    }
    f->store_buffer(reinterpret_cast<const uint8_t *>(p_gaussians.ptr()), uint64_t(p_gaussians.size()) * sizeof(Gaussian));
    f.unref();
    return true;
}

} // namespace

TEST_CASE("[GaussianSplatting][WorldIO] StagedFileChunkPayloadSource contiguous snapshot reads requested bytes") {
    const String path = _make_world_io_fixture_path("staged_contiguous");
    const Vector<Gaussian> gaussians = build_staged_payload_gaussians(16);
    REQUIRE(write_staged_payload_fixture(path, gaussians));

    Ref<StagedFileChunkPayloadSource> source;
    source.instantiate();
    source->configure(path, 0, 0, gaussians.size(), 0, 0, 0, AABB());

    LocalVector<Gaussian> snapshot;
    LocalVector<Vector3> sh_high;
    uint32_t sh_first = 0;
    uint32_t sh_high_count = 0;
    source->reset_io_counters();
    const bool ok = source->capture_chunk_snapshot(4, 5, snapshot, sh_high, sh_first, sh_high_count);

    CHECK(ok);
    CHECK_EQ(snapshot.size(), 5);
    if (snapshot.size() == 5) {
        CHECK(snapshot[0].position.is_equal_approx(gaussians[4].position));
        CHECK(snapshot[4].position.is_equal_approx(gaussians[8].position));
        // The full Gaussian struct is persisted/read verbatim, so scale/rotation/
        // opacity/sh_dc/sh_1 (all derived deterministically in make_gaussian) must
        // round-trip exactly. sh_high is empty in this fixture so it is not asserted.
        _check_attributes_round_trip(snapshot[0], gaussians[4]);
        _check_attributes_round_trip(snapshot[4], gaussians[8]);
    }
    CHECK_EQ(source->get_bytes_requested(), uint64_t(5) * sizeof(Gaussian));
    CHECK_EQ(source->get_bytes_read(), uint64_t(5) * sizeof(Gaussian));
    CHECK_EQ(source->get_file_open_count(), 1);

    LocalVector<Gaussian> second_snapshot;
    const bool second_ok = source->capture_chunk_snapshot(8, 1, second_snapshot, sh_high, sh_first, sh_high_count);
    CHECK(second_ok);
    CHECK_EQ(second_snapshot.size(), 1);
    if (second_snapshot.size() == 1) {
        CHECK(second_snapshot[0].position.is_equal_approx(gaussians[8].position));
        _check_attributes_round_trip(second_snapshot[0], gaussians[8]);
    }
    CHECK_EQ(source->get_bytes_requested(), uint64_t(6) * sizeof(Gaussian));
    CHECK_EQ(source->get_bytes_read(), uint64_t(6) * sizeof(Gaussian));
    CHECK_EQ(source->get_file_open_count(), 1);

    _remove_world_io_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO] StagedFileChunkPayloadSource sparse indexed snapshot avoids full span read") {
    const String path = _make_world_io_fixture_path("staged_sparse_indexed");
    const Vector<Gaussian> gaussians = build_staged_payload_gaussians(128);
    REQUIRE(write_staged_payload_fixture(path, gaussians));

    Ref<StagedFileChunkPayloadSource> source;
    source.instantiate();
    source->configure(path, 0, 0, gaussians.size(), 0, 0, 0, AABB());

    const uint32_t indices[] = { 2, 80, 127, 3 };
    LocalVector<Gaussian> snapshot;
    LocalVector<Vector3> sh_high;
    uint32_t sh_first = 0;
    uint32_t sh_high_count = 0;
    source->reset_io_counters();
    const bool ok = source->capture_indexed_chunk_snapshot(indices, 4, snapshot, sh_high, sh_first, sh_high_count);

    CHECK(ok);
    CHECK_EQ(snapshot.size(), 4);
    if (snapshot.size() == 4) {
        CHECK(snapshot[0].position.is_equal_approx(gaussians[2].position));
        CHECK(snapshot[1].position.is_equal_approx(gaussians[80].position));
        CHECK(snapshot[2].position.is_equal_approx(gaussians[127].position));
        CHECK(snapshot[3].position.is_equal_approx(gaussians[3].position));
        // The sparse-indexed path must also preserve every per-splat attribute,
        // each output slot mapping back to its requested source index. sh_high is
        // empty in this fixture so it is not asserted.
        _check_attributes_round_trip(snapshot[0], gaussians[2]);
        _check_attributes_round_trip(snapshot[1], gaussians[80]);
        _check_attributes_round_trip(snapshot[2], gaussians[127]);
        _check_attributes_round_trip(snapshot[3], gaussians[3]);
    }
    CHECK_EQ(source->get_bytes_requested(), uint64_t(4) * sizeof(Gaussian));
    CHECK_EQ(source->get_bytes_read(), uint64_t(4) * sizeof(Gaussian));
    CHECK_LT(source->get_bytes_read(), uint64_t(126) * sizeof(Gaussian));
    CHECK_EQ(source->get_file_open_count(), 1);

    _remove_world_io_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO] gsplatworld direct format saver/loader") {
    GsplatWorldCompressionSettingGuard compression_guard(false);
    // This test bypasses ResourceLoader/ResourceSaver to test our format directly
    Ref<GaussianData> gaussian_data;
    gaussian_data.instantiate();
    Vector<Gaussian> gaussians = build_gaussians();
    gaussian_data->set_gaussians(gaussians);

    // Verify set_gaussians worked
    CHECK_MESSAGE(gaussian_data->get_count() == 4, "GaussianData should have 4 splats after set_gaussians");

    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(gaussian_data);
    world->set_bounds(gaussian_data->get_aabb());
    world->set_static_chunks(build_chunks());

    Dictionary metadata;
    metadata[StringName("lod_levels")] = 2;
    world->set_metadata(metadata);

    // Verify world has data before save
    CHECK_MESSAGE(world->get_gaussian_data()->get_count() == 4, "World should have 4 splats before save");
    CHECK_MESSAGE(world->get_static_chunks().size() == 2, "World should have 2 chunks before save");

    const String path = _make_world_io_fixture_path("direct_test");

    // Use our format saver directly
    ResourceFormatSaverGaussianSplatWorld saver;
    const Error save_err = saver.save(world, path);
    CHECK_MESSAGE(save_err == OK, "Direct saver should succeed");
    if (save_err != OK) {
        return;
    }

    // Verify file was written
    CHECK_MESSAGE(FileAccess::exists(path), "File should exist after save");
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
    CHECK_MESSAGE(f.is_valid(), "Should be able to open saved file");
    if (f.is_valid()) {
        const uint64_t file_size = f->get_length();
        // Expected: 104 header + 4*144 gaussian + 2*56 chunk_table + 4*4 indices + metadata
        MESSAGE("File size: ", file_size, " bytes");
        CHECK_MESSAGE(file_size > 104, "File should be larger than just header");
        f.unref();
    }

    // Use our format loader directly
    ResourceFormatLoaderGaussianSplatWorld loader;
    Error load_err = OK;
    Ref<Resource> loaded_res = loader.load(path, "", &load_err);
    CHECK_MESSAGE(load_err == OK, "Direct loader should succeed");
    CHECK_MESSAGE(loaded_res.is_valid(), "Loaded resource should be valid");

    Ref<GaussianSplatWorld> loaded = loaded_res;
    CHECK_MESSAGE(loaded.is_valid(), "Loaded resource should be GaussianSplatWorld");
    if (!loaded.is_valid()) {
        return;
    }

    Ref<GaussianData> loaded_data = loaded->get_gaussian_data();
    CHECK_MESSAGE(loaded_data.is_null(), "Default uncompressed load should not materialize GaussianData");
    CHECK_FALSE(loaded->has_resident_gaussian_data());
    CHECK(loaded->has_chunk_payload_source());
    CHECK(loaded->is_payload_source_backed());
    CHECK(loaded->has_renderable_payload());
    CHECK_EQ(loaded->get_splat_count(), 4);

    const Vector<GaussianSplatRenderer::StaticChunk> &chunks = loaded->get_static_chunks();
    MESSAGE("Loaded chunk count: ", chunks.size());
    CHECK_EQ(chunks.size(), 2);
    CHECK_EQ(loaded->get_sh_first_order_count(), 1);
    CHECK_EQ(loaded->get_sh_high_order_count(), 0);

    Ref<ChunkPayloadSource> payload_source = loaded->get_chunk_payload_source();
    CHECK(payload_source.is_valid());
    CHECK(payload_source->is_valid());
    CHECK_EQ(payload_source->get_count(), 4);
    if (payload_source.is_valid()) {
        LocalVector<Gaussian> snapshot;
        LocalVector<Vector3> sh_high;
        uint32_t sh_first = 0;
        uint32_t sh_high_count = 0;
        const bool snapshot_ok = payload_source->capture_chunk_snapshot(0, 2,
                snapshot, sh_high, sh_first, sh_high_count);
        CHECK(snapshot_ok);
        CHECK_EQ(snapshot.size(), 2);
        if (snapshot.size() >= 1) {
            CHECK(snapshot[0].position.is_equal_approx(gaussians[0].position));
            CHECK_EQ(gaussian_get_palette_id(snapshot[0].painterly_meta), 17);
            CHECK_EQ(gaussian_get_brush_override_id(snapshot[0].painterly_meta), 300);
        }

        LocalVector<Gaussian> indexed_snapshot;
        const bool indexed_ok = chunks.size() >= 2 &&
                payload_source->capture_indexed_chunk_snapshot(chunks[1].indices.ptr(), chunks[1].indices.size(),
                        indexed_snapshot, sh_high, sh_first, sh_high_count);
        CHECK(indexed_ok);
        CHECK_EQ(indexed_snapshot.size(), 2);
        if (indexed_snapshot.size() >= 2) {
            CHECK(indexed_snapshot[0].position.is_equal_approx(gaussians[2].position));
            CHECK(indexed_snapshot[1].position.is_equal_approx(gaussians[3].position));
        }
    }

    const String resave_path = _make_world_io_fixture_path("direct_resave");
    const Error resave_err = saver.save(loaded, resave_path);
    CHECK_MESSAGE(resave_err == OK, "Saving a source-backed gsplatworld should write a preserved streamable payload.");
    CHECK_FALSE_MESSAGE(loaded->has_resident_gaussian_data(),
            "Generic save must not mutate a source-backed world into resident GaussianData.");
    CHECK(loaded->has_chunk_payload_source());
    CHECK(loaded->get_payload_mode() == String("streamable_uncompressed"));
    CHECK_MESSAGE(FileAccess::exists(resave_path), "Resaved source-backed gsplatworld should exist.");
    CHECK_FALSE_MESSAGE(_world_io_file_has_compression_flag(resave_path),
            "Generic save of a streamable source-backed world must stay uncompressed.");
    Error resaved_load_err = OK;
    Ref<Resource> resaved_res = loader.load(resave_path, "", &resaved_load_err);
    CHECK(resaved_load_err == OK);
    Ref<GaussianSplatWorld> resaved_world = resaved_res;
    CHECK(resaved_world.is_valid());
    if (resaved_world.is_valid()) {
        CHECK_FALSE(resaved_world->has_resident_gaussian_data());
        CHECK(resaved_world->has_chunk_payload_source());
        CHECK(resaved_world->is_streamable_payload());
    }
    _remove_world_io_fixture(resave_path);

    Ref<GaussianSplatWorld> resident_loaded = loader.load_resident(path, &load_err);
    CHECK_MESSAGE(load_err == OK, "Explicit resident loader should succeed");
    CHECK(resident_loaded.is_valid());
    if (resident_loaded.is_valid()) {
        Ref<GaussianData> resident_data = resident_loaded->get_gaussian_data();
        CHECK(resident_data.is_valid());
        CHECK(resident_loaded->has_resident_gaussian_data());
        CHECK_FALSE(resident_loaded->has_chunk_payload_source());
        CHECK_EQ(resident_loaded->get_splat_count(), 4);
        if (resident_data.is_valid()) {
            const Gaussian g0 = resident_data->get_gaussian(0);
            CHECK_EQ(gaussian_get_palette_id(g0.painterly_meta), 17);
            CHECK_EQ(gaussian_get_brush_override_id(g0.painterly_meta), 300);
        }
    }

    // Cleanup
    _remove_world_io_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO] gsplatworld save/load round-trip") {
    GsplatWorldCompressionSettingGuard compression_guard(false);
    // Check what resource type the loader returns for our extension
    String detected_type = ResourceLoader::get_resource_type("test.gsplatworld");
    MESSAGE("ResourceLoader detected type for .gsplatworld: '", detected_type, "'");

    // Register saver for test scope (workaround for test init order).
    GsplatWorldSaverGuard saver_guard;
    MESSAGE("Explicitly registered gsplatworld saver for test (at_front=true)");

    Ref<GaussianData> gaussian_data;
    gaussian_data.instantiate();
    Vector<Gaussian> gaussians = build_gaussians();
    gaussian_data->set_gaussians(gaussians);

    // Verify set_gaussians worked
    CHECK_MESSAGE(gaussian_data->get_count() == 4, "GaussianData should have 4 splats after set_gaussians");
    if (gaussian_data->get_count() != 4) {
        MESSAGE("CRITICAL: set_gaussians failed! Expected 4, got ", gaussian_data->get_count());
        return;
    }

    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(gaussian_data);
    world->set_bounds(gaussian_data->get_aabb());
    world->set_static_chunks(build_chunks());

    Dictionary metadata;
    metadata[StringName("lod_levels")] = 2;
    metadata[StringName("author")] = String("test");
    world->set_metadata(metadata);

    const String path = _make_world_io_fixture_path("roundtrip");
    const Error save_err = ResourceSaver::save(world, path);
    CHECK_MESSAGE(save_err == OK, "Saving gsplatworld should succeed.");
    if (save_err != OK) {
        return;
    }

    // Check what ResourceSaver actually wrote
    {
        Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
        if (f.is_valid()) {
            uint64_t fsize = f->get_length();
            MESSAGE("File saved via ResourceSaver, size: ", fsize, " bytes");
            if (fsize >= 4) {
                uint32_t magic = f->get_32();
                MESSAGE("File magic: 0x", String::num_int64(magic, 16), " (expected 0x57505347 for GSPW)");
                if (magic == 0x57505347) {
                    MESSAGE("File has correct GSPW magic - our saver was used!");
                    f->seek(12);
                    uint32_t splat_count = f->get_32();
                    MESSAGE("splat_count in file: ", splat_count);
                } else {
                    MESSAGE("File has WRONG magic - ResourceSaver used different saver!");
                    // Read first 50 bytes as text to see format
                    f->seek(0);
                    PackedByteArray first_bytes = f->get_buffer(MIN(fsize, (uint64_t)50));
                    String preview = String::utf8((const char*)first_bytes.ptr(), first_bytes.size());
                    MESSAGE("File preview: '", preview.substr(0, 50), "'");
                }
            }
            f.unref();
        }
    }

    Ref<GaussianSplatWorld> loaded = ResourceLoader::load(path);
    CHECK_MESSAGE(loaded.is_valid(), "Loading gsplatworld should succeed.");
    if (!loaded.is_valid()) {
        return;
    }

    Ref<GaussianData> loaded_data = loaded->get_gaussian_data();
    CHECK(loaded_data.is_null());
    CHECK_FALSE(loaded->has_resident_gaussian_data());
    CHECK(loaded->has_chunk_payload_source());
    CHECK_EQ(loaded->get_splat_count(), uint32_t(gaussians.size()));

    const Error materialize_err = loaded->materialize_resident_gaussian_data();
    CHECK(materialize_err == OK);
    loaded_data = loaded->get_gaussian_data();
    CHECK(loaded_data.is_valid());
    if (loaded_data.is_valid() && loaded_data->get_count() > 0) {
        MESSAGE("Materialized gaussian count via ResourceLoader: ", loaded_data->get_count());
        CHECK_EQ(loaded_data->get_count(), gaussians.size());
        const Gaussian g0 = loaded_data->get_gaussian(0);
        CHECK(g0.position.is_equal_approx(gaussians[0].position));
        CHECK(g0.sh_dc.is_equal_approx(gaussians[0].sh_dc));
        CHECK_EQ(gaussian_get_palette_id(g0.painterly_meta), 17);
        CHECK_EQ(gaussian_get_brush_override_id(g0.painterly_meta), 300);
    }

    const Vector<GaussianSplatRenderer::StaticChunk> &chunks = loaded->get_static_chunks();
    MESSAGE("Loaded chunk count via ResourceLoader: ", chunks.size());
    CHECK_EQ(chunks.size(), 2);
    if (chunks.size() >= 2) {
        CHECK_EQ(chunks[0].indices.size(), 2);
        CHECK_EQ(chunks[0].indices[0], 0);
        CHECK_EQ(chunks[1].indices[1], 3);
    }

    Dictionary loaded_metadata = loaded->get_metadata();
    CHECK(loaded_metadata.has(StringName("lod_levels")));
    CHECK(int(loaded_metadata[StringName("lod_levels")]) == 2);

    // Cleanup
    _remove_world_io_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO] compressed gsplatworld remains resident-only") {
    GsplatWorldCompressionSettingGuard compression_guard(true);

    Ref<GaussianData> gaussian_data;
    gaussian_data.instantiate();
    Vector<Gaussian> gaussians;
    gaussians.resize(16); // > 1KB so the saver attempts compression.
    for (int i = 0; i < gaussians.size(); i++) {
        gaussians.write[i] = make_gaussian(Vector3(float(i), 0.0f, 0.0f), Color(1.0f, 0.0f, 0.0f, 1.0f));
    }
    gaussian_data->set_gaussians(gaussians);

    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(gaussian_data);
    world->set_bounds(gaussian_data->get_aabb());

    const String path = _make_world_io_fixture_path("compressed_resident");
    ResourceFormatSaverGaussianSplatWorld saver;
    const Error save_err = saver.save_resident_compressed(world, path);
    CHECK(save_err == OK);
    if (save_err != OK) {
        return;
    }

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
    REQUIRE(f.is_valid());
    f->seek(8); // magic + version
    const uint32_t flags = f->get_32();
    CHECK_MESSAGE((flags & (1u << 4u)) != 0u, "Fixture should exercise the compressed load path");

    ResourceFormatLoaderGaussianSplatWorld loader;
    Error load_err = OK;
    Ref<Resource> loaded_res = loader.load(path, "", &load_err);
    CHECK(load_err == OK);
    Ref<GaussianSplatWorld> loaded = loaded_res;
    CHECK(loaded.is_valid());
    if (loaded.is_valid()) {
        CHECK(loaded->has_resident_gaussian_data());
        CHECK_FALSE(loaded->has_chunk_payload_source());
        CHECK_EQ(loaded->get_splat_count(), uint32_t(gaussians.size()));
        CHECK(loaded->get_gaussian_data().is_valid());
    }

    _remove_world_io_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO] explicit resident-uncompressed save loads resident by default") {
    Ref<GaussianData> gaussian_data;
    gaussian_data.instantiate();
    Vector<Gaussian> gaussians = build_gaussians();
    gaussian_data->set_gaussians(gaussians);

    Ref<GaussianSplatWorld> source_world;
    source_world.instantiate();
    source_world->set_gaussian_data(gaussian_data);
    source_world->set_bounds(gaussian_data->get_aabb());
    source_world->set_static_chunks(build_chunks());

    const String streamable_path = _make_world_io_fixture_path("resident_uncompressed_source");
    const String resident_path = _make_world_io_fixture_path("resident_uncompressed_export");
    ResourceFormatSaverGaussianSplatWorld saver;
    REQUIRE(saver.save(source_world, streamable_path) == OK);

    ResourceFormatLoaderGaussianSplatWorld loader;
    Error load_err = OK;
    Ref<GaussianSplatWorld> source_backed = loader.load(streamable_path, "", &load_err);
    REQUIRE(load_err == OK);
    REQUIRE(source_backed.is_valid());
    CHECK(source_backed->has_chunk_payload_source());
    CHECK_FALSE(source_backed->has_resident_gaussian_data());

    REQUIRE(saver.save_resident_uncompressed(source_backed, resident_path) == OK);
    CHECK_FALSE_MESSAGE(_world_io_file_has_compression_flag(resident_path),
            "Explicit resident-uncompressed save must remain uncompressed.");

    Ref<GaussianSplatWorld> resident_loaded = loader.load(resident_path, "", &load_err);
    REQUIRE(load_err == OK);
    REQUIRE(resident_loaded.is_valid());
    CHECK(resident_loaded->has_resident_gaussian_data());
    CHECK_FALSE(resident_loaded->has_chunk_payload_source());
    CHECK_FALSE(resident_loaded->is_streamable_payload());
    REQUIRE(resident_loaded->get_gaussian_data().is_valid());
    CHECK_EQ(resident_loaded->get_gaussian_data()->get_count(), gaussians.size());

    _remove_world_io_fixture(streamable_path);
    _remove_world_io_fixture(resident_path);
}

TEST_CASE("[GaussianSplatting][WorldIO] explicit compressed save compresses small resident payloads") {
    Ref<GaussianData> gaussian_data;
    gaussian_data.instantiate();
    Vector<Gaussian> gaussians;
    gaussians.resize(1);
    gaussians.write[0] = make_gaussian(Vector3(0.25f, 0.5f, 0.75f), Color(0.25f, 0.5f, 0.75f, 1.0f));
    gaussian_data->set_gaussians(gaussians);

    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(gaussian_data);
    world->set_bounds(gaussian_data->get_aabb());

    const String path = _make_world_io_fixture_path("small_explicit_compressed");
    ResourceFormatSaverGaussianSplatWorld saver;
    const Error save_err = saver.save_resident_compressed(world, path);
    CHECK_MESSAGE(save_err == OK, "Explicit resident-compressed save must compress even <=1024-byte payloads.");
    if (save_err != OK) {
        _remove_world_io_fixture(path);
        return;
    }
    CHECK_MESSAGE(_world_io_file_has_compression_flag(path),
            "Explicit resident-compressed small payload save must not silently fall back to uncompressed output.");

    ResourceFormatLoaderGaussianSplatWorld loader;
    Error load_err = OK;
    Ref<Resource> loaded_res = loader.load(path, "", &load_err);
    CHECK(load_err == OK);
    Ref<GaussianSplatWorld> loaded = loaded_res;
    CHECK(loaded.is_valid());
    if (loaded.is_valid()) {
        CHECK(loaded->has_resident_gaussian_data());
        CHECK_FALSE(loaded->has_chunk_payload_source());
        CHECK_EQ(loaded->get_payload_mode(), String("resident_only"));
        REQUIRE(loaded->get_gaussian_data().is_valid());
        CHECK_EQ(loaded->get_gaussian_data()->get_count(), 1);
    }

    _remove_world_io_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO] explicit compressed save fails when no compressed payload can be produced") {
    Ref<GaussianData> gaussian_data;
    gaussian_data.instantiate();
    gaussian_data->resize(0);

    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(gaussian_data);

    const String path = _make_world_io_fixture_path("empty_explicit_compressed");
    ResourceFormatSaverGaussianSplatWorld saver;
    const Error save_err = saver.save_resident_compressed(world, path);
    CHECK_MESSAGE(save_err != OK,
            "Explicit resident-compressed save must fail loudly when there is no gaussian payload to compress.");
    CHECK_FALSE_MESSAGE(FileAccess::exists(path),
            "Failed explicit resident-compressed save must not leave an uncompressed world file behind.");

    _remove_world_io_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO] ResourceSaver preserves streamable payload when compression setting is enabled") {
    GsplatWorldSaverGuard saver_guard;
    GsplatWorldCompressionSettingGuard compression_guard(true);

    Ref<GaussianData> gaussian_data;
    gaussian_data.instantiate();
    Vector<Gaussian> gaussians;
    gaussians.resize(16);
    for (int i = 0; i < gaussians.size(); i++) {
        gaussians.write[i] = make_gaussian(Vector3(float(i), float(i % 4), 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f));
    }
    gaussian_data->set_gaussians(gaussians);

    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(gaussian_data);
    world->set_bounds(gaussian_data->get_aabb());
    world->set_static_chunks(build_chunks());

    const String source_path = _make_world_io_fixture_path("resource_saver_source");
    const String resave_path = _make_world_io_fixture_path("resource_saver_resave");
    const Error save_err = ResourceSaver::save(world, source_path);
    CHECK(save_err == OK);
    CHECK_FALSE_MESSAGE(_world_io_file_has_compression_flag(source_path),
            "Generic ResourceSaver::save() must not use ambient compression for world files.");
    if (save_err != OK) {
        _remove_world_io_fixture(source_path);
        _remove_world_io_fixture(resave_path);
        return;
    }

    Ref<GaussianSplatWorld> loaded = ResourceLoader::load(source_path, "GaussianSplatWorld",
            ResourceFormatLoader::CACHE_MODE_IGNORE);
    CHECK(loaded.is_valid());
    if (!loaded.is_valid()) {
        _remove_world_io_fixture(source_path);
        _remove_world_io_fixture(resave_path);
        return;
    }
    CHECK(loaded->is_streamable_payload());
    CHECK_FALSE(loaded->has_resident_gaussian_data());

    const Error resave_err = ResourceSaver::save(loaded, resave_path);
    CHECK(resave_err == OK);
    CHECK_FALSE(loaded->has_resident_gaussian_data());
    CHECK(loaded->is_streamable_payload());
    CHECK_FALSE_MESSAGE(_world_io_file_has_compression_flag(resave_path),
            "Save/load/save of a streamable world must remain uncompressed even when compression is enabled globally.");

    Ref<GaussianSplatWorld> reloaded = ResourceLoader::load(resave_path, "GaussianSplatWorld",
            ResourceFormatLoader::CACHE_MODE_IGNORE);
    CHECK(reloaded.is_valid());
    if (reloaded.is_valid()) {
        CHECK_FALSE(reloaded->has_resident_gaussian_data());
        CHECK(reloaded->has_chunk_payload_source());
        CHECK(reloaded->is_streamable_payload());
        CHECK(reloaded->get_payload_mode() == String("streamable_uncompressed"));
    }

    ResourceFormatSaverGaussianSplatWorld explicit_saver;
    const String compressed_path = _make_world_io_fixture_path("resource_saver_explicit_compressed");
    CHECK(explicit_saver.save_resident_compressed(world, compressed_path) == OK);
    CHECK_MESSAGE(_world_io_file_has_compression_flag(compressed_path),
            "Compressed world export must be explicit resident-only output.");
    _remove_world_io_fixture(compressed_path);
    _remove_world_io_fixture(source_path);
    _remove_world_io_fixture(resave_path);
}

namespace {

struct MalformedWorldHeader {
    uint32_t magic = 0x57505347u; // 'GSPW'
    uint32_t version = 1u;
    uint32_t flags = 0u;
    uint32_t splat_count = 0u;
    uint32_t sh_degree = 0u;
    uint32_t sh_first_order = 0u;
    uint32_t sh_high_order = 0u;
    Vector3 bounds_pos;
    Vector3 bounds_size;
    uint32_t chunk_count = 0u;
    uint64_t gaussian_offset = 104u;
    uint64_t sh_offset = 0u;
    uint64_t chunk_table_offset = 0u;
    uint64_t indices_offset = 0u;
    uint64_t metadata_offset = 0u;
    uint64_t metadata_size = 0u;
};

bool _write_malformed_world(const String &p_path, const MalformedWorldHeader &p_header, uint64_t p_pad_bytes = 0) {
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
    if (f.is_null()) {
        return false;
    }
    f->store_32(p_header.magic);
    f->store_32(p_header.version);
    f->store_32(p_header.flags);
    f->store_32(p_header.splat_count);
    f->store_32(p_header.sh_degree);
    f->store_32(p_header.sh_first_order);
    f->store_32(p_header.sh_high_order);
    f->store_float(p_header.bounds_pos.x);
    f->store_float(p_header.bounds_pos.y);
    f->store_float(p_header.bounds_pos.z);
    f->store_float(p_header.bounds_size.x);
    f->store_float(p_header.bounds_size.y);
    f->store_float(p_header.bounds_size.z);
    f->store_32(p_header.chunk_count);
    f->store_64(p_header.gaussian_offset);
    f->store_64(p_header.sh_offset);
    f->store_64(p_header.chunk_table_offset);
    f->store_64(p_header.indices_offset);
    f->store_64(p_header.metadata_offset);
    f->store_64(p_header.metadata_size);
    for (uint64_t i = 0; i < p_pad_bytes; i++) {
        f->store_8(0);
    }
    f.unref();
    return true;
}

} // namespace

TEST_CASE("[GaussianSplatting][WorldIO] gsplatworld rejects metadata range overflow via fits_within") {
    // Exercises the F5 fits_within helper on the metadata range. The OLD code
    // computed `metadata_offset + metadata_size > file_len`, which wraps when
    // metadata_size is near UINT64_MAX, producing a small sum that slips past
    // the comparison. The new helper rejects via the offset/size split.
    const String path = _make_world_io_fixture_path("malformed_fits_within");

    const uint64_t pad = 256u;
    const uint64_t file_len = 104u + pad;

    MalformedWorldHeader hdr;
    hdr.flags = 1u << 0u; // kFlagHasMetadata
    hdr.splat_count = 0u;
    hdr.gaussian_offset = 104u;
    hdr.metadata_offset = file_len - 4u;
    hdr.metadata_size = UINT64_MAX - 8u;
    REQUIRE(_write_malformed_world(path, hdr, pad));

    ResourceFormatLoaderGaussianSplatWorld loader;
    Error err = OK;
    Ref<Resource> result = loader.load(path, "", &err);
    CHECK_EQ(err, ERR_FILE_CORRUPT);
    CHECK_FALSE(result.is_valid());

    _remove_world_io_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO] gsplatworld rejects high SH count without high SH flag") {
    const String path = _make_world_io_fixture_path("malformed_sh_flag_mismatch");

    MalformedWorldHeader hdr;
    hdr.flags = 0u;
    hdr.splat_count = 1u;
    hdr.sh_high_order = 1u;
    hdr.gaussian_offset = 104u;
    REQUIRE(_write_malformed_world(path, hdr, sizeof(Gaussian)));

    ResourceFormatLoaderGaussianSplatWorld loader;
    Error err = OK;
    Ref<Resource> result = loader.load(path, "", &err);
    CHECK_EQ(err, ERR_FILE_CORRUPT);
    CHECK_FALSE(result.is_valid());

    _remove_world_io_fixture(path);
}

// F6 (checked_mul_u64 on splat_count * sh_high_order * sizeof(Vector3)) is
// defense-in-depth and not reachable through the public header inputs: the
// sh_high_order <= 12 cap caps sh_bytes at splat_count * 144, which fits in
// uint64 for any uint32 splat_count. No standalone test is added — the path
// would require directly invoking the helper.

TEST_CASE("[GaussianSplatting][WorldIO] gsplatworld rejects chunk-index byte-count overflow") {
    // Crafts a chunk record whose indices_offset+index_count produces a
    // total_indices near UINT64_MAX/4, so total_indices * sizeof(uint32_t)
    // wraps. The new checked_mul_u64 + fits_within guards the read.
    const String path = _make_world_io_fixture_path("malformed_chunk_indices_overflow");

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE(f.is_valid());

    const uint32_t flags = 1u << 2u; // kFlagHasChunks
    const uint32_t chunk_count = 1u;
    const uint64_t header_size = 104u;
    const uint64_t chunk_table_offset = header_size;
    const uint64_t chunk_table_bytes = uint64_t(chunk_count) * 56u;
    const uint64_t indices_offset_field = chunk_table_offset + chunk_table_bytes;
    const uint64_t pad_after_chunk_table = 64u;
    const uint64_t file_len = chunk_table_offset + chunk_table_bytes + pad_after_chunk_table;

    f->store_32(0x57505347u); // magic
    f->store_32(1u); // version
    f->store_32(flags);
    f->store_32(0u); // splat_count
    f->store_32(0u); // sh_degree
    f->store_32(0u); // sh_first_order
    f->store_32(0u); // sh_high_order
    for (int i = 0; i < 6; i++) {
        f->store_float(0.0f); // bounds pos+size
    }
    f->store_32(chunk_count);
    f->store_64(header_size); // gaussian_offset
    f->store_64(0u); // sh_offset
    f->store_64(chunk_table_offset);
    f->store_64(indices_offset_field);
    f->store_64(0u); // metadata_offset
    f->store_64(0u); // metadata_size

    // Chunk record: indices_offset + index_count = UINT64_MAX/4 + 5, so
    // total_indices * 4 wraps past UINT64_MAX.
    const uint64_t bogus_indices_offset = (UINT64_MAX / 4u) - 5u;
    const uint32_t bogus_index_count = 10u;
    for (int i = 0; i < 9; i++) {
        f->store_float(0.0f); // bounds_pos, bounds_size, center
    }
    f->store_float(0.0f); // radius
    f->store_64(bogus_indices_offset);
    f->store_32(bogus_index_count);
    f->store_32(0u); // reserved

    for (uint64_t i = 0; i < pad_after_chunk_table; i++) {
        f->store_8(0);
    }
    f.unref();

    REQUIRE(FileAccess::exists(path));
    REQUIRE(FileAccess::get_size(path) == file_len);

    ResourceFormatLoaderGaussianSplatWorld loader;
    Error err = OK;
    Ref<Resource> result = loader.load(path, "", &err);
    CHECK_EQ(err, ERR_FILE_CORRUPT);
    CHECK_FALSE(result.is_valid());

    _remove_world_io_fixture(path);
}
