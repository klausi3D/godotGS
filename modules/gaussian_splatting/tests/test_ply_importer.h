#pragma once

#include "test_macros.h"
#include "../io/ply_loader.h"
#include "../io/resource_importer_ply.h"
#include "../io/streaming_chunk_bake.h"
#include "../io/gaussian_splat_world_io.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gaussian_splat_asset.h"
#include "synthetic_ply_writer.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

namespace {

// Minimal PLY file content for testing
const char *MINIMAL_PLY_CONTENT = R"(ply
format binary_little_endian 1.0
element vertex 2
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float opacity
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
)";

String _make_ply_fixture_path(const String &p_prefix) {
    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String base_temp = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : ".";
    return base_temp.path_join("godotgs_ply_fixture_" + p_prefix + "_" + itos(ticks) + ".ply");
}

void _remove_ply_fixture(const String &p_path) {
    DirAccess::remove_absolute(p_path);
}

} // namespace

TEST_CASE("[GaussianSplatting][PLY] parse minimal binary PLY") {
    // Write test PLY to temp file
    const String path = _make_ply_fixture_path("minimal");

    // Create minimal PLY with header + binary data
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    CHECK_MESSAGE(f.is_valid(), "Should create test PLY file");
    if (!f.is_valid()) return;

    f->store_string(MINIMAL_PLY_CONTENT);

    // Write 2 vertices of binary data (14 floats each = 56 bytes per vertex)
    // Vertex 0: position (0,0,0), scale (1,1,1), rotation identity (w,x,y,z), opacity 1, dc (1,0,0)
    float v0[14] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f};
    f->store_buffer((const uint8_t *)v0, sizeof(v0));

    // Vertex 1: position (1,0,0), scale (1,1,1), rotation identity (w,x,y,z), opacity 1, dc (0,1,0)
    float v1[14] = {1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    f->store_buffer((const uint8_t *)v1, sizeof(v1));
    f.unref();

    // Load using PLYLoader
    PLYLoader loader;
    Error err = loader.load_file(path);

    CHECK_MESSAGE(err == OK, "PLY load should succeed");

    Ref<GaussianData> data = loader.get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Data should be valid");
    if (data.is_valid()) {
        CHECK_EQ(data->get_count(), 2);

        if (data->get_count() >= 2) {
            // Check first gaussian
            CHECK(data->get_gaussian(0).position.is_equal_approx(Vector3(0, 0, 0)));

            // Check second gaussian
            CHECK(data->get_gaussian(1).position.is_equal_approx(Vector3(1, 0, 0)));
        }
    }

    // Cleanup
    _remove_ply_fixture(path);
}

TEST_CASE("[GaussianSplatting][PLY] parse ASCII PLY") {
    const String path = _make_ply_fixture_path("ascii");

    const char *ascii_ply = R"(ply
format ascii 1.0
element vertex 1
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float opacity
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
0.5 0.5 0.5 1.0 1.0 1.0 1.0 0.0 0.0 0.0 0.8 0.5 0.5 0.5
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    CHECK_MESSAGE(f.is_valid(), "Should create ASCII PLY file");
    if (!f.is_valid()) return;

    f->store_string(ascii_ply);
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);

    CHECK_MESSAGE(err == OK, "ASCII PLY load should succeed");

    Ref<GaussianData> data = loader.get_gaussian_data();
    CHECK_MESSAGE(data.is_valid(), "Data should be valid");
    if (data.is_valid()) {
        CHECK_EQ(data->get_count(), 1);

        if (data->get_count() >= 1) {
            CHECK(data->get_gaussian(0).position.is_equal_approx(Vector3(0.5f, 0.5f, 0.5f)));
        }
    }

    // Cleanup
    _remove_ply_fixture(path);
}

TEST_CASE("[GaussianSplatting][PLYLoader] Cache version mismatch forces re-parse") {
    // Write a minimal binary PLY fixture using the same pattern as other tests.
    const String ply_path = _make_ply_fixture_path("cache_version");

    {
        Ref<FileAccess> f = FileAccess::open(ply_path, FileAccess::WRITE);
        REQUIRE_MESSAGE(f.is_valid(), "Should create test PLY file");
        f->store_string(MINIMAL_PLY_CONTENT);
        float v0[14] = { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0 };
        f->store_buffer((const uint8_t *)v0, sizeof(v0));
        float v1[14] = { 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0 };
        f->store_buffer((const uint8_t *)v1, sizeof(v1));
    }

    // First load: parses PLY, writes .gsplatcache.
    {
        PLYLoader loader;
        Error err = loader.load_file(ply_path);
        CHECK_MESSAGE(err == OK, "Initial PLY load should succeed");
        CHECK(loader.get_splat_count() == 2);
    }

    // Tamper with the cache version: load the .gsplatcache, change version, re-save.
    // Use the format loader/saver directly because .gsplatcache is not a globally
    // recognised extension (by design — it's internal to PLYLoader).
    const String cache_path = ply_path.get_basename() + ".gsplatcache";
    if (FileAccess::exists(cache_path)) {
        ResourceFormatLoaderGaussianSplatWorld format_loader;
        Error load_err = OK;
        Ref<GaussianSplatWorld> world = format_loader.load_resident(cache_path, &load_err);
        REQUIRE_MESSAGE(world.is_valid(), "Cache should be a valid GaussianSplatWorld");

        Dictionary metadata = world->get_metadata();
        metadata[StringName("cache_version")] = 9999; // Wrong version
        world->set_metadata(metadata);
        ResourceFormatSaverGaussianSplatWorld format_saver;
        format_saver.save(world, cache_path);

        // Second load: cache should be rejected because of version mismatch.
        PLYLoader loader;
        Error err = loader.load_file(ply_path);
        CHECK_MESSAGE(err == OK, "PLY load should still succeed (re-parse fallback)");
        CHECK(loader.get_splat_count() == 2);

        Dictionary stats = loader.get_load_statistics();
        if (stats.has("cache_hit")) {
            CHECK_MESSAGE(!(bool)stats["cache_hit"], "Version-mismatched cache should not be a cache hit");
        }
    } else {
        MESSAGE("Cache file not created (caching may be disabled); skipping version guard test");
    }

    // Cleanup.
    _remove_ply_fixture(ply_path);
    DirAccess::remove_absolute(cache_path);
}

TEST_CASE("[GaussianSplatting][PLYLoader] Cache source path mismatch forces re-parse") {
    const String ply_path = _make_ply_fixture_path("cache_source_path");

    {
        Ref<FileAccess> f = FileAccess::open(ply_path, FileAccess::WRITE);
        REQUIRE_MESSAGE(f.is_valid(), "Should create test PLY file");
        f->store_string(MINIMAL_PLY_CONTENT);
        float v0[14] = { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0 };
        f->store_buffer((const uint8_t *)v0, sizeof(v0));
        float v1[14] = { 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0 };
        f->store_buffer((const uint8_t *)v1, sizeof(v1));
    }

    {
        PLYLoader loader;
        Error err = loader.load_file(ply_path);
        CHECK_MESSAGE(err == OK, "Initial PLY load should succeed");
        CHECK(loader.get_splat_count() == 2);
    }

    const String cache_path = ply_path.get_basename() + ".gsplatcache";
    if (FileAccess::exists(cache_path)) {
        ResourceFormatLoaderGaussianSplatWorld format_loader;
        Error load_err = OK;
        Ref<GaussianSplatWorld> world = format_loader.load_resident(cache_path, &load_err);
        REQUIRE_MESSAGE(world.is_valid(), "Cache should be a valid GaussianSplatWorld");

        Dictionary metadata = world->get_metadata();
        metadata[StringName("cache_source_path")] = ply_path + ".other";
        world->set_metadata(metadata);
        ResourceFormatSaverGaussianSplatWorld format_saver;
        format_saver.save_resident_uncompressed(world, cache_path);

        PLYLoader loader;
        Error err = loader.load_file(ply_path);
        CHECK_MESSAGE(err == OK, "PLY load should still succeed (re-parse fallback)");
        CHECK(loader.get_splat_count() == 2);

        Dictionary stats = loader.get_load_statistics();
        if (stats.has("cache_hit")) {
            CHECK_MESSAGE(!(bool)stats["cache_hit"], "Source-path-mismatched cache should not be a cache hit");
        }
    } else {
        MESSAGE("Cache file not created (caching may be disabled); skipping source path guard test");
    }

    _remove_ply_fixture(ply_path);
    DirAccess::remove_absolute(cache_path);
}

TEST_CASE("[GaussianSplatting][PLYLoader] Legacy sibling gsplatworld caches are ignored") {
    const String ply_path = _make_ply_fixture_path("legacy_cache_migration");

    {
        Ref<FileAccess> f = FileAccess::open(ply_path, FileAccess::WRITE);
        REQUIRE_MESSAGE(f.is_valid(), "Should create test PLY file");
        f->store_string(MINIMAL_PLY_CONTENT);
        float v0[14] = { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0 };
        f->store_buffer((const uint8_t *)v0, sizeof(v0));
        float v1[14] = { 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0 };
        f->store_buffer((const uint8_t *)v1, sizeof(v1));
    }

    {
        PLYLoader loader;
        Error err = loader.load_file(ply_path);
        CHECK_MESSAGE(err == OK, "Initial PLY load should succeed");
        CHECK(loader.get_splat_count() == 2);
    }

    const String cache_path = ply_path.get_basename() + ".gsplatcache";
    const String legacy_cache_path = ply_path.get_basename() + ".gsplatworld";

    if (FileAccess::exists(cache_path)) {
        DirAccess::remove_absolute(legacy_cache_path);
        REQUIRE_MESSAGE(DirAccess::rename_absolute(cache_path, legacy_cache_path) == OK,
                "Renaming the cache to the legacy .gsplatworld path should succeed");
        CHECK_FALSE(FileAccess::exists(cache_path));
        CHECK(FileAccess::exists(legacy_cache_path));

        PLYLoader loader;
        Error err = loader.load_file(ply_path);
        CHECK_MESSAGE(err == OK, "PLY load should re-parse raw data instead of accepting the legacy cache path");
        CHECK(loader.get_splat_count() == 2);

        Dictionary stats = loader.get_load_statistics();
        if (stats.has("cache_hit")) {
            CHECK_MESSAGE(!(bool)stats["cache_hit"], "Legacy sibling .gsplatworld files must not count as a cache hit");
        }

        CHECK_MESSAGE(FileAccess::exists(cache_path),
                "Raw re-parse should recreate the canonical .gsplatcache");
        CHECK_MESSAGE(FileAccess::exists(legacy_cache_path),
                "Ignoring the legacy sibling cache must not silently delete user-authored .gsplatworld files");
    } else {
        MESSAGE("Cache file not created (caching may be disabled); skipping legacy sibling-cache rejection test");
    }

    _remove_ply_fixture(ply_path);
    DirAccess::remove_absolute(cache_path);
    DirAccess::remove_absolute(legacy_cache_path);
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] reject vertex_count out of int range") {
    const String path = _make_ply_fixture_path("oversized_count");

    const char *oversized_ply = R"(ply
format binary_little_endian 1.0
element vertex 9999999999
property float x
property float y
property float z
end_header
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create oversized PLY fixture");
    f->store_string(oversized_ply);
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT,
            "PLY with vertex_count beyond int range should be rejected");

    _remove_ply_fixture(path);
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] reject header missing end_header sentinel") {
    const String path = _make_ply_fixture_path("missing_end_header");

    // Smoke test for F4 — exercise the load path end-to-end with a header
    // that lacks the `end_header` sentinel. Old code without the new
    // `found_end_header` guard would consume input until EOF in the header
    // loop and then fail later on a short binary read, ending in
    // ERR_FILE_CORRUPT at a different stage; the new guard catches the
    // problem at parse_header() before binary read begins. Through the
    // public load_file() API the two stages are not separately
    // distinguishable, so this case asserts the outcome (corrupt file
    // refused) rather than pinning the specific stage. A precise
    // stage-level pin would need a parse_header() seam.
    const char *truncated_ply = R"(ply
format binary_little_endian 1.0
element vertex 4
property float x
property float y
property float z
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create truncated PLY fixture");
    f->store_string(truncated_ply);
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT,
            "PLY without end_header sentinel should be rejected");

    _remove_ply_fixture(path);
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] reject unknown property type token") {
    const String path = _make_ply_fixture_path("unknown_property_type");

    // Payload bytes follow so old code (which would set unknown
    // type's size=0 and then read garbage at the wrong offsets) could
    // otherwise complete parsing without ERR_FILE_CORRUPT. The new
    // guard must reject at the header stage.
    const char *unknown_type_ply = R"(ply
format binary_little_endian 1.0
element vertex 4
property float x
property custom_type y
property float z
end_header
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create unknown-type PLY fixture");
    f->store_string(unknown_type_ply);
    for (int i = 0; i < 4 * 3; i++) {
        f->store_float(0.0f);
    }
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT,
            "PLY with unknown property type should be rejected");

    _remove_ply_fixture(path);
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] reject vertex-element property list (binary, issue #511)") {
    // A `property list` inside the vertex element makes each vertex row a
    // variable length (count + N entries), so the per-vertex stride is not
    // fixed. The pre-fix loader `continue`d past the list line: it dropped the
    // property from BOTH header.properties and the stride sum, computed
    // vertex_size from the surviving fixed properties only, and then read every
    // vertex after the first at the wrong offset -> load "succeeded" with
    // garbage. The fix rejects at parse_header instead.
    const String path = _make_ply_fixture_path("vertex_list_binary");

    const char *list_ply = R"(ply
format binary_little_endian 1.0
element vertex 2
property float x
property float y
property float z
property list uchar int extra_indices
property float opacity
end_header
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create vertex-list PLY fixture");
    f->store_string(list_ply);
    // Plausible payload bytes so the OLD code (which would size the vertex from
    // x/y/z/opacity only) could read to completion without a short-read error.
    f->set_big_endian(false);
    for (int i = 0; i < 32; i++) {
        f->store_8((uint8_t)i);
    }
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT,
            "PLY whose vertex element declares a 'property list' must be rejected, not loaded as garbage");

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] reject vertex-element property list (ASCII, issue #511)") {
    // Same defect on the ASCII path: parse_header is shared, so a vertex-element
    // list property must be rejected before any row is parsed.
    const String path = _make_ply_fixture_path("vertex_list_ascii");

    const char *list_ply = R"(ply
format ascii 1.0
element vertex 1
property float x
property float y
property float z
property list uchar int extra_indices
end_header
0.5 0.5 0.5 2 10 20
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create ASCII vertex-list PLY fixture");
    f->store_string(list_ply);
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT,
            "ASCII PLY whose vertex element declares a 'property list' must be rejected");

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] fixed-size element before vertex parses correctly (binary, issue #512)") {
    // A legal PLY may declare another element (here a 1-row `camera`) BEFORE
    // `vertex`. In a binary PLY the camera's data is stored first, so the
    // pre-fix loader (which assumed vertex data starts at header_size) read the
    // camera bytes as vertex 0 -> silent misparse. The fix skips the fixed-size
    // preceding element's data and parses the vertex block correctly.
    const String path = _make_ply_fixture_path("preceding_fixed_binary");

    const char *preceding_ply = R"(ply
format binary_little_endian 1.0
element camera 1
property float view_px
property float view_py
property float view_pz
element vertex 2
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float opacity
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create preceding-element PLY fixture");
    f->store_string(preceding_ply);
    f->set_big_endian(false);
    // Camera row: distinct non-zero values. If the skip were missing, vertex 0
    // would read these as its position.
    f->store_float(7.0f);
    f->store_float(8.0f);
    f->store_float(9.0f);
    // Vertex 0: position (0,0,0); Vertex 1: position (1,0,0). Same field order
    // as MINIMAL_PLY_CONTENT.
    float v0[14] = { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0 };
    float v1[14] = { 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0 };
    for (int i = 0; i < 14; i++) {
        f->store_float(v0[i]);
    }
    for (int i = 0; i < 14; i++) {
        f->store_float(v1[i]);
    }
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == OK, "PLY with a fixed-size element before 'vertex' should load correctly");

    Ref<GaussianData> data = loader.get_gaussian_data();
    REQUIRE_MESSAGE(data.is_valid(), "Loaded GaussianData should be valid");
    REQUIRE(data->get_count() == 2);
    CHECK(data->get_gaussian(0).position.is_equal_approx(Vector3(0, 0, 0)));
    CHECK(data->get_gaussian(1).position.is_equal_approx(Vector3(1, 0, 0)));
    // Regression assertion: vertex 0 must NOT be the camera row (7,8,9), which
    // is what the pre-fix loader would have read.
    CHECK_MESSAGE(!data->get_gaussian(0).position.is_equal_approx(Vector3(7, 8, 9)),
            "Vertex 0 must not be the preceding element's data (offset-skip regression)");

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] fixed-size element before vertex parses correctly (ASCII, issue #512)") {
    // ASCII companion to the binary preceding-element case: the loader must
    // skip the preceding element's data rows (one line each) before reading
    // vertex rows.
    const String path = _make_ply_fixture_path("preceding_fixed_ascii");

    const char *preceding_ply = R"(ply
format ascii 1.0
element camera 1
property float view_px
property float view_py
property float view_pz
element vertex 1
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float opacity
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
7.0 8.0 9.0
0.5 0.5 0.5 1.0 1.0 1.0 1.0 0.0 0.0 0.0 0.8 0.5 0.5 0.5
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create ASCII preceding-element PLY fixture");
    f->store_string(preceding_ply);
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == OK, "ASCII PLY with a preceding element should load correctly");

    Ref<GaussianData> data = loader.get_gaussian_data();
    REQUIRE_MESSAGE(data.is_valid(), "Loaded GaussianData should be valid");
    REQUIRE(data->get_count() == 1);
    CHECK(data->get_gaussian(0).position.is_equal_approx(Vector3(0.5f, 0.5f, 0.5f)));
    CHECK_MESSAGE(!data->get_gaussian(0).position.is_equal_approx(Vector3(7, 8, 9)),
            "Vertex 0 must not be the preceding camera row (ASCII row-skip regression)");

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] reject variable-length element before vertex (issue #512)") {
    // A preceding element with a `property list` has variable-length rows, so
    // the byte offset to the vertex block cannot be computed without parsing
    // every row. The loader rejects rather than guessing (fail closed).
    const String path = _make_ply_fixture_path("preceding_list");

    const char *preceding_list_ply = R"(ply
format binary_little_endian 1.0
element face 1
property list uchar int vertex_indices
element vertex 1
property float x
property float y
property float z
end_header
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create preceding-list PLY fixture");
    f->store_string(preceding_list_ply);
    f->set_big_endian(false);
    // face row (count=3 + 3 int indices) followed by one vertex of floats.
    f->store_8((uint8_t)3);
    f->store_32((uint32_t)0);
    f->store_32((uint32_t)1);
    f->store_32((uint32_t)2);
    f->store_float(0.0f);
    f->store_float(0.0f);
    f->store_float(0.0f);
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == ERR_FILE_CORRUPT,
            "PLY with a variable-length element before 'vertex' must be rejected, not misparsed");

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] happy-path control: vertex-first with trailing element loads (issue #511/#512)") {
    // Positive control proving the hardened parser does NOT over-reject: a
    // normal vertex-first PLY that also declares a TRAILING element (with a
    // list property) after `vertex` must still load. Trailing elements are
    // stored after the vertex block and are never read, so their presence — and
    // even a `property list` among them — must be ignored, not rejected.
    const String path = _make_ply_fixture_path("trailing_element_control");

    const char *trailing_ply = R"(ply
format binary_little_endian 1.0
element vertex 2
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float opacity
property float f_dc_0
property float f_dc_1
property float f_dc_2
element face 1
property list uchar int vertex_indices
end_header
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create trailing-element PLY fixture");
    f->store_string(trailing_ply);
    f->set_big_endian(false);
    float v0[14] = { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0 };
    float v1[14] = { 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0 };
    for (int i = 0; i < 14; i++) {
        f->store_float(v0[i]);
    }
    for (int i = 0; i < 14; i++) {
        f->store_float(v1[i]);
    }
    // Trailing face payload (never read by the loader).
    f->store_8((uint8_t)3);
    f->store_32((uint32_t)0);
    f->store_32((uint32_t)1);
    f->store_32((uint32_t)2);
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == OK, "Vertex-first PLY with a trailing element must still load");

    Ref<GaussianData> data = loader.get_gaussian_data();
    REQUIRE_MESSAGE(data.is_valid(), "Loaded GaussianData should be valid");
    REQUIRE(data->get_count() == 2);
    CHECK(data->get_gaussian(0).position.is_equal_approx(Vector3(0, 0, 0)));
    CHECK(data->get_gaussian(1).position.is_equal_approx(Vector3(1, 0, 0)));

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLY] big-endian binary round-trip") {
    const String path = _make_ply_fixture_path("big_endian");

    const char *big_endian_header = R"(ply
format binary_big_endian 1.0
element vertex 1
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float opacity
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create big-endian PLY fixture");
    f->store_string(big_endian_header);

    // Native little-endian floats; we manually byte-swap each one to produce
    // a canonical big-endian payload that the loader must un-swap.
    const float native_values[14] = {
        2.5f, -3.25f, 7.75f, // position
        1.0f, 1.0f, 1.0f, // scales
        1.0f, 0.0f, 0.0f, 0.0f, // rotation (w,x,y,z)
        0.5f, // opacity
        0.25f, 0.5f, 0.75f // dc
    };
    for (float native : native_values) {
        uint32_t bits;
        memcpy(&bits, &native, sizeof(uint32_t));
        bits = BSWAP32(bits);
        uint8_t bytes[4];
        memcpy(bytes, &bits, sizeof(bytes));
        f->store_buffer(bytes, sizeof(bytes));
    }
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == OK, "Big-endian PLY load should succeed");

    Ref<GaussianData> data = loader.get_gaussian_data();
    REQUIRE(data.is_valid());
    REQUIRE(data->get_count() == 1);
    const Gaussian g = data->get_gaussian(0);
    CHECK(g.position.is_equal_approx(Vector3(2.5f, -3.25f, 7.75f)));

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] integer-typed properties convert to float (regression: silent-zero bug)") {
    // Regression guard for the pre-2026-07-07 loader bug where
    // PLYLoader::read_float_property returned 0.0 for every non-float property
    // type. Integer property types (char/uchar/short/ushort/int/uint) are legal
    // PLY, so a binary PLY that stored positions/axes/ages as integers decoded
    // them all as zeros — a silent corruption that still "loaded successfully".
    //
    // This little-endian fixture maps all six integer type families onto
    // PLYLoader pass-through fields (position x/y/z, brush_axis_u/v, stroke_age)
    // so the loaded values can be compared directly with no activation transform:
    //   x            = int    (int32)  = -5      (signed, negative)
    //   y            = short  (int16)  = 1000    (wider than uint8, positive)
    //   z            = uchar  (uint8)  = 250     (unsigned byte)
    //   brush_axis_u = char   (int8)   = -7      (signed byte, negative)
    //   brush_axis_v = uint   (uint32) = 100000  (wider than int16, positive)
    //   stroke_age   = ushort (uint16) = 40000   (wider than int16, positive)
    const String path = _make_ply_fixture_path("integer_props_le");

    const char *int_props_header = R"(ply
format binary_little_endian 1.0
element vertex 1
property int x
property short y
property uchar z
property char brush_axis_u
property uint brush_axis_v
property ushort stroke_age
end_header
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create integer-typed PLY fixture");
    f->store_string(int_props_header);
    f->set_big_endian(false);
    f->store_32((uint32_t)(int32_t)-5);        // x   : int32
    f->store_16((uint16_t)(int16_t)1000);      // y   : int16
    f->store_8((uint8_t)250);                  // z   : uint8
    f->store_8((uint8_t)(int8_t)-7);           // u   : int8
    f->store_32((uint32_t)100000);             // v   : uint32
    f->store_16((uint16_t)40000);              // age : uint16
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == OK, "Integer-typed PLY load should succeed");

    Ref<GaussianData> data = loader.get_gaussian_data();
    REQUIRE_MESSAGE(data.is_valid(), "Loaded GaussianData should be valid");
    REQUIRE(data->get_count() == 1);

    const Gaussian g = data->get_gaussian(0);

    // Core assertion: integer properties decode to their true numeric value,
    // preserving sign and width — NOT the silent-zero the old code produced.
    CHECK(g.position.is_equal_approx(Vector3(-5.0f, 1000.0f, 250.0f)));
    CHECK(g.brush_axes.is_equal_approx(Vector2(-7.0f, 100000.0f)));
    CHECK(Math::is_equal_approx(g.stroke_age, 40000.0f));

    // Explicit regression assertion: the old read_float_property returned 0.0
    // for these integer types, so every field above would have been zero.
    CHECK_MESSAGE(!g.position.is_equal_approx(Vector3(0, 0, 0)),
            "Integer positions must not collapse to zero (silent-zero regression)");

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLY][MalformedCorpus] big-endian integer-typed properties byte-swap correctly") {
    // Companion to the little-endian integer test: exercises the endianness
    // (byte-swap) branch of read_float_property for multi-byte integer types.
    //   x = int    (int32)  = -1000000 (signed, negative, needs 32-bit swap)
    //   y = short  (int16)  = -300     (signed, negative, needs 16-bit swap)
    //   z = ushort (uint16) = 40000    (unsigned, > int16 max, needs 16-bit swap)
    const String path = _make_ply_fixture_path("integer_props_be");

    const char *int_props_header = R"(ply
format binary_big_endian 1.0
element vertex 1
property int x
property short y
property ushort z
end_header
)";

    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    REQUIRE_MESSAGE(f.is_valid(), "Should create big-endian integer PLY fixture");
    f->store_string(int_props_header);
    f->set_big_endian(true);
    f->store_32((uint32_t)(int32_t)-1000000); // x : int32 (big-endian bytes)
    f->store_16((uint16_t)(int16_t)-300);     // y : int16 (big-endian bytes)
    f->store_16((uint16_t)40000);             // z : uint16 (big-endian bytes)
    f.unref();

    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == OK, "Big-endian integer-typed PLY load should succeed");

    Ref<GaussianData> data = loader.get_gaussian_data();
    REQUIRE_MESSAGE(data.is_valid(), "Loaded GaussianData should be valid");
    REQUIRE(data->get_count() == 1);

    const Gaussian g = data->get_gaussian(0);
    CHECK(g.position.is_equal_approx(Vector3(-1000000.0f, -300.0f, 40000.0f)));

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLYLoader] Stale v1 cache is rejected after integer-decode fix (regression)") {
    // A .gsplatcache written by the pre-2026-07-07 loader (PLY_CACHE_VERSION 1)
    // may hold GaussianData decoded with the integer-as-zero bug. After bumping
    // PLY_CACHE_VERSION to 2, load_file() must REJECT such a stale v1 cache and
    // re-parse the raw PLY, so already-imported integer-property PLYs get
    // corrected data WITHOUT the user manually deleting caches.
    const String ply_path = _make_ply_fixture_path("stale_v1_cache");

    {
        Ref<FileAccess> f = FileAccess::open(ply_path, FileAccess::WRITE);
        REQUIRE_MESSAGE(f.is_valid(), "Should create test PLY file");
        f->store_string(MINIMAL_PLY_CONTENT);
        float v0[14] = { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0 };
        f->store_buffer((const uint8_t *)v0, sizeof(v0));
        float v1[14] = { 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0 };
        f->store_buffer((const uint8_t *)v1, sizeof(v1));
    }

    // First load: parses the PLY and writes a current-version (.gsplatcache v2).
    {
        PLYLoader loader;
        Error err = loader.load_file(ply_path);
        CHECK_MESSAGE(err == OK, "Initial PLY load should succeed");
        CHECK(loader.get_splat_count() == 2);
    }

    const String cache_path = ply_path.get_basename() + ".gsplatcache";
    if (FileAccess::exists(cache_path)) {
        // Rewrite the cache metadata tagged as the OLD pre-fix version (1),
        // simulating a cache left behind by the buggy loader.
        ResourceFormatLoaderGaussianSplatWorld format_loader;
        Error load_err = OK;
        Ref<GaussianSplatWorld> world = format_loader.load_resident(cache_path, &load_err);
        REQUIRE_MESSAGE(world.is_valid(), "Cache should be a valid GaussianSplatWorld");

        Dictionary metadata = world->get_metadata();
        metadata[StringName("cache_version")] = 1; // pre-fix loader's cache version
        world->set_metadata(metadata);
        ResourceFormatSaverGaussianSplatWorld format_saver;
        format_saver.save(world, cache_path);

        // Second load: the stale v1 cache must be rejected (re-parse fallback),
        // so the fixed integer decode runs instead of reusing zeroed data.
        PLYLoader loader;
        Error err = loader.load_file(ply_path);
        CHECK_MESSAGE(err == OK, "PLY load should still succeed (re-parse fallback)");
        CHECK(loader.get_splat_count() == 2);

        Dictionary stats = loader.get_load_statistics();
        if (stats.has("cache_hit")) {
            CHECK_MESSAGE(!(bool)stats["cache_hit"],
                    "Stale v1 cache must not count as a cache hit after the integer-decode fix");
        }
    } else {
        MESSAGE("Cache file not created (caching may be disabled); skipping stale v1 cache rejection test");
    }

    _remove_ply_fixture(ply_path);
    DirAccess::remove_absolute(cache_path);
}

TEST_CASE("[GaussianSplatting][PLY] opacity survives import - logit round-trip (regression: all-0.5 bug)") {
    // Regression guard for the pre-2026-05-03 importer bug where PLY imports
    // never wrote opacity_logits, so every splat read back as sigmoid(0)=0.5.
    // The synthetic writer logit-encodes Gaussian::opacity (an ACTIVATED [0,1]
    // value) exactly like a real 3DGS PLY, and PLYLoader re-applies sigmoid on
    // load. A correct round-trip must recover the distinct input opacities;
    // the bug would collapse them all to ~0.5.
    const String path = _make_ply_fixture_path("opacity_roundtrip");

    // Four splats with distinct, known activated opacities spanning the range.
    // Avoid 0 and 1 because logit() diverges there (the writer clamps to
    // [1e-6, 1-1e-6], which would otherwise inflate round-trip error).
    const float expected_opacities[4] = { 0.1f, 0.35f, 0.65f, 0.9f };

    LocalVector<Gaussian> splats;
    splats.resize(4);
    for (int i = 0; i < 4; i++) {
        Gaussian g;
        g.position = Vector3((float)i, 0.0f, 0.0f);
        g.scale = Vector3(1.0f, 1.0f, 1.0f);       // unit scale -> log(1) = 0
        g.rotation = Quaternion();                  // identity (w,x,y,z) = (1,0,0,0)
        g.sh_dc = Color(0.5f, 0.5f, 0.5f, 1.0f);    // some valid DC color
        g.normal = Vector3(0.0f, 0.0f, 1.0f);
        g.area = 1.0f;
        g.opacity = expected_opacities[i];          // ACTIVATED [0,1] opacity
        splats[i] = g;
    }

    // Write the synthetic PLY (logit-encodes opacity internally). No SH band-1,
    // no normals are needed for this opacity-focused round-trip.
    REQUIRE_MESSAGE(TestGaussianSplatting::write_gaussian_ply(path, splats, false, false),
            "Should write synthetic opacity PLY fixture");

    // Load it back through the real importer path.
    PLYLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == OK, "PLY load should succeed");

    Ref<GaussianData> data = loader.get_gaussian_data();
    REQUIRE_MESSAGE(data.is_valid(), "Loaded GaussianData should be valid");
    REQUIRE(data->get_count() == 4);

    // Opacity flows: input activated -> writer logit -> loader sigmoid -> here.
    // It is stored as a float32 through this path (no 8-bit quantization), so a
    // tight tolerance would pass; we use a generous epsilon of 0.02 to absorb
    // the writer's [1e-6, 1-1e-6] logit clamp and float round-trip error.
    const float epsilon = 0.02f;
    float min_opacity = 1.0f;
    float max_opacity = 0.0f;
    for (int i = 0; i < 4; i++) {
        const float recovered = data->get_gaussian(i).opacity;
        CHECK_MESSAGE(Math::abs(recovered - expected_opacities[i]) <= epsilon,
                vformat("Splat %d opacity should round-trip: expected %f, got %f",
                        i, expected_opacities[i], recovered));
        min_opacity = MIN(min_opacity, recovered);
        max_opacity = MAX(max_opacity, recovered);
    }

    // Explicit regression assertion: the recovered opacities must NOT all be
    // approximately equal. The all-0.5 bug would produce a spread near zero;
    // the true inputs span 0.1..0.9 (spread 0.8).
    CHECK_MESSAGE(max_opacity - min_opacity > 0.3f,
            "Recovered opacities must not collapse to a single value (all-0.5 regression)");

    _remove_ply_fixture(path);
    DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

TEST_CASE("[GaussianSplatting][PLY] opacity survives ResourceImporterPLY -> asset get_opacities (regression: zero-filled logits)") {
    // Companion to the PLYLoader round-trip test above. The PLYLoader-only test
    // exercises parse_binary_data() -> GaussianData::opacity, which already
    // populates opacity from the PLY logit, so it does NOT cover the shipped-once
    // bug: ResourceImporterPLY built a GaussianSplatAsset and (before the fix at
    // resource_importer_ply.cpp:383-388) left opacity_logits at their zero-filled
    // resize() defaults. GaussianSplatAsset::get_opacities() prefers logits over
    // color.a, so zero logits sigmoid to 0.5 for EVERY splat regardless of the
    // real stored opacity. This test drives the real importer -> save -> load ->
    // get_opacities() path so it FAILS if that population is removed.
#ifndef TOOLS_ENABLED
    MESSAGE("Skipping - ResourceImporterPLY (and thus the import->asset path) requires TOOLS_ENABLED");
    return;
#else
    // Same distinct, known activated opacities as the loader test so the two
    // assertions share one mental model. Endpoints avoid 0/1 where logit diverges.
    const float expected_opacities[4] = { 0.1f, 0.35f, 0.65f, 0.9f };

    LocalVector<Gaussian> splats;
    splats.resize(4);
    for (int i = 0; i < 4; i++) {
        Gaussian g;
        g.position = Vector3((float)i, 0.0f, 0.0f);
        g.scale = Vector3(1.0f, 1.0f, 1.0f);
        g.rotation = Quaternion(); // identity (w,x,y,z) = (1,0,0,0)
        g.sh_dc = Color(0.5f, 0.5f, 0.5f, 1.0f);
        g.normal = Vector3(0.0f, 0.0f, 1.0f);
        g.area = 1.0f;
        g.opacity = expected_opacities[i]; // ACTIVATED [0,1] opacity
        splats[i] = g;
    }

    // The importer reads via ResourceFormat/ResourceSaver, so anchor the source
    // and the imported .res under user:// (the proven path for headless importer
    // tests) rather than the OS temp dir used for the loader-only fixtures.
    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String source_path = "user://godotgs_opacity_importer_" + itos(ticks) + ".ply";
    const String save_base_path = "user://godotgs_opacity_importer_" + itos(ticks) + "_asset";

    REQUIRE_MESSAGE(TestGaussianSplatting::write_gaussian_ply(source_path, splats, false, false),
            "Should write synthetic opacity PLY fixture for the importer");

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    // Keep all four splats, in order, with no thumbnail: ultra preset +
    // max_splats=0 + density=1.0 means final_count == original_count and no
    // density merge or opacity sort, so get_opacities()[i] lines up with
    // expected_opacities[i]. normalize_opacity is the importer default (true).
    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("quality/max_splats"), 0);
    options.insert(StringName("quality/density_multiplier"), 1.0);
    options.insert(StringName("processing/sort_by_opacity"), false);
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &metadata_variant);
    CHECK_MESSAGE(import_err == OK, "ResourceImporterPLY::import should succeed for the synthetic opacity PLY");

    if (import_err == OK) {
        Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
        REQUIRE_MESSAGE(asset.is_valid(), "Imported GaussianSplatAsset should load from disk");
        REQUIRE(int(asset->get_splat_count()) == 4);

        // This is the regression-critical call: it sigmoids opacity_logits and
        // only falls back to color.a when logits are absent. The importer always
        // sizes opacity_logits to splat_count, so all-zero logits (the bug) would
        // yield sigmoid(0)=0.5 for every splat here and shadow color.a entirely.
        PackedFloat32Array opacities = asset->get_opacities();
        REQUIRE(opacities.size() == 4);

        const float epsilon = 0.02f;
        float min_opacity = 1.0f;
        float max_opacity = 0.0f;
        for (int i = 0; i < 4; i++) {
            const float recovered = opacities[i];
            CHECK_MESSAGE(Math::abs(recovered - expected_opacities[i]) <= epsilon,
                    vformat("Splat %d opacity should survive import->asset: expected %f, got %f",
                            i, expected_opacities[i], recovered));
            min_opacity = MIN(min_opacity, recovered);
            max_opacity = MAX(max_opacity, recovered);
        }

        // The all-0.5 bug collapses the spread to ~0; the true inputs span
        // 0.1..0.9. This is the assertion that fails if opacity_logits are left
        // at their zero-filled defaults (resource_importer_ply.cpp:383-388 removed).
        CHECK_MESSAGE(max_opacity - min_opacity > 0.3f,
                "Imported opacities must not collapse to a single value (zero-filled logit regression)");
    }

    DirAccess::remove_absolute(source_path);
    DirAccess::remove_absolute(source_path.get_basename() + ".gsplatcache");
    DirAccess::remove_absolute(save_base_path + ".res");
#endif // TOOLS_ENABLED
}

// ---------------------------------------------------------------------------
// GS-PERF-PRUNE slice 2b (issue #456): import-time importance pruning wiring.
// ---------------------------------------------------------------------------

namespace {

// Build N splats whose importance (opacity * max|scale|) strictly increases with
// the source index (opacity fixed at 1.0, scale increasing), so a ratio prune
// keeps a known suffix, and each splat is identifiable by its distinct integer
// position.x. PLY scales are stored as full-precision log(scale), so importance
// ordering is exact (no quantization).
LocalVector<Gaussian> _make_prune_splats(uint32_t p_count) {
    LocalVector<Gaussian> splats;
    splats.resize(p_count);
    for (uint32_t i = 0; i < p_count; i++) {
        Gaussian g;
        g.position = Vector3(float(i), 0.0f, 0.0f);
        const float s = 0.1f * float(i + 1); // strictly increasing, distinct
        g.scale = Vector3(s, s, s);
        g.rotation = Quaternion();
        g.sh_dc = Color(0.5f, 0.5f, 0.5f, 1.0f);
        g.normal = Vector3(0.0f, 0.0f, 1.0f);
        g.area = 1.0f;
        g.opacity = 1.0f;
        splats[i] = g;
    }
    return splats;
}

} // namespace

TEST_CASE("[GaussianSplatting][PLY] importer default prune options are a no-op (Ultra byte-identity)") {
#ifndef TOOLS_ENABLED
    MESSAGE("Skipping - ResourceImporterPLY requires TOOLS_ENABLED");
    return;
#else
    const uint32_t kCount = 16;
    LocalVector<Gaussian> splats = _make_prune_splats(kCount);

    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String source_path = "user://godotgs_ply_prune_noop_" + itos(ticks) + ".ply";
    const String save_base_path = "user://godotgs_ply_prune_noop_" + itos(ticks) + "_asset";

    REQUIRE_MESSAGE(TestGaussianSplatting::write_gaussian_ply(source_path, splats, false, false),
            "Should write synthetic PLY fixture");

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    // Ultra + no prune options -> default (1.0 / 0.0) no-op. No density merge, no
    // sort, no max cap, so the output is the source in order.
    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("quality/max_splats"), 0);
    options.insert(StringName("quality/density_multiplier"), 1.0);
    options.insert(StringName("processing/sort_by_opacity"), false);
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &metadata_variant);
    CHECK_MESSAGE(import_err == OK, "PLY import at default (no-op) prune options should succeed");

    if (import_err == OK) {
        Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
        REQUIRE_MESSAGE(asset.is_valid(), "Imported GaussianSplatAsset should load from disk");
        CHECK_EQ(int(asset->get_splat_count()), int(kCount));

        // No-op path must not drop or reorder: every source splat survives in
        // order (position.x == source index).
        PackedFloat32Array positions = asset->get_positions();
        REQUIRE(positions.size() == int(kCount) * 3);
        for (uint32_t i = 0; i < kCount; i++) {
            CHECK(Math::is_equal_approx(positions[int(i) * 3 + 0], float(i)));
        }

        Dictionary md = metadata_variant;
        CHECK_EQ(int(md.get(StringName("original_splat_count"), -1)), int(kCount));
        CHECK_EQ(int(md.get(StringName("pre_prune_splat_count"), -1)), int(kCount));
        CHECK_EQ(int(md.get(StringName("splat_count"), -1)), int(kCount));
    }

    DirAccess::remove_absolute(source_path);
    DirAccess::remove_absolute(source_path.get_basename() + ".gsplatcache");
    DirAccess::remove_absolute(save_base_path + ".res");
#endif // TOOLS_ENABLED
}

TEST_CASE("[GaussianSplatting][PLY] importer prune_ratio 0.5 keeps the highest-importance half") {
#ifndef TOOLS_ENABLED
    MESSAGE("Skipping - ResourceImporterPLY requires TOOLS_ENABLED");
    return;
#else
    const uint32_t kCount = 16;
    const int kExpectedKept = 8; // round(16 * 0.5)
    LocalVector<Gaussian> splats = _make_prune_splats(kCount);

    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String source_path = "user://godotgs_ply_prune_half_" + itos(ticks) + ".ply";
    const String save_base_path = "user://godotgs_ply_prune_half_" + itos(ticks) + "_asset";

    REQUIRE_MESSAGE(TestGaussianSplatting::write_gaussian_ply(source_path, splats, false, false),
            "Should write synthetic PLY fixture for the prune test");

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("quality/max_splats"), 0);
    options.insert(StringName("quality/density_multiplier"), 1.0);
    options.insert(StringName("processing/sort_by_opacity"), false);
    options.insert(StringName("processing/prune_ratio"), 0.5);
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &metadata_variant);
    CHECK_MESSAGE(import_err == OK, "PLY import with prune_ratio 0.5 should succeed");

    if (import_err == OK) {
        Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
        REQUIRE_MESSAGE(asset.is_valid(), "Pruned GaussianSplatAsset should load from disk");
        CHECK_EQ(int(asset->get_splat_count()), kExpectedKept);

        // Dual counts: original == 16, pre-prune == 16, final splat_count == 8.
        Dictionary md = metadata_variant;
        CHECK_EQ(int(md.get(StringName("original_splat_count"), -1)), int(kCount));
        CHECK_EQ(int(md.get(StringName("pre_prune_splat_count"), -1)), int(kCount));
        CHECK_EQ(int(md.get(StringName("splat_count"), -1)), kExpectedKept);

        // The kept splats are the highest-importance suffix (source indices 8..15),
        // identifiable by their distinct integer position.x.
        PackedFloat32Array positions = asset->get_positions();
        REQUIRE(positions.size() == kExpectedKept * 3);
        for (int j = 0; j < kExpectedKept; j++) {
            const float x = positions[j * 3 + 0];
            CHECK_MESSAGE(x >= 7.5f,
                    vformat("Kept splat %d has position.x=%f; expected a high-importance splat (x >= 8)", j, x));
        }

        // Chunk-bake consistency: the baked streaming-chunk records must describe
        // the PRUNED arrays (sum of chunk counts == pruned splat_count == 8), not
        // the stale pre-prune 16. A stale bake here is a data-corruption bug.
        Vector<StreamingChunkBakeRecord> records;
        REQUIRE(StreamingChunkBakeIO::deserialize_records(asset->get_streaming_chunk_records(), records));
        REQUIRE(records.size() > 0);
        uint32_t total_in_chunks = 0;
        uint32_t expected_start = 0;
        for (int r = 0; r < records.size(); r++) {
            CHECK_EQ(records[r].start_idx, expected_start);
            total_in_chunks += records[r].count;
            expected_start += records[r].count;
        }
        CHECK_EQ(int(total_in_chunks), kExpectedKept);
    }

    DirAccess::remove_absolute(source_path);
    DirAccess::remove_absolute(source_path.get_basename() + ".gsplatcache");
    DirAccess::remove_absolute(save_base_path + ".res");
#endif // TOOLS_ENABLED
}
