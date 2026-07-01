#pragma once

#include "test_macros.h"
#include "../io/ply_loader.h"
#include "../io/resource_importer_ply.h"
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

TEST_CASE("[GaussianSplatting][PLY] reject vertex_count out of int range") {
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

TEST_CASE("[GaussianSplatting][PLY] reject header missing end_header sentinel") {
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

TEST_CASE("[GaussianSplatting][PLY] reject unknown property type token") {
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
