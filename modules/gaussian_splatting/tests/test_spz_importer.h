#pragma once

#include "test_macros.h"
#include "../io/spz_loader.h"
#include "../io/resource_importer_spz.h"
#include "../io/streaming_chunk_bake.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "synthetic_spz_writer.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

namespace TestGaussianSplattingSPZ {

// Build N synthetic splats whose importance (opacity * max|scale|) is strictly
// increasing in the source index, so a ratio prune keeps a known suffix and each
// survivor is identifiable by its distinct integer position.x. Opacity is held
// at 1.0 so importance ordering is driven purely by scale.
inline LocalVector<TestGaussianSplatting::SyntheticSpzSplat> _make_spz_splats(uint32_t p_count) {
    LocalVector<TestGaussianSplatting::SyntheticSpzSplat> splats;
    splats.resize(p_count);
    for (uint32_t i = 0; i < p_count; i++) {
        TestGaussianSplatting::SyntheticSpzSplat s;
        s.position = Vector3(float(i), 0.0f, 0.0f);
        s.opacity = 1.0f;
        s.color = Color(0.5f, 0.5f, 0.5f, 1.0f);
        // Geometric scale spread guarantees strictly-increasing, distinct
        // log-encoded scale bytes -> strictly-increasing importance.
        const float v = 0.05f * Math::exp(0.3f * float(i));
        s.scale = Vector3(v, v, v);
        s.rotation = Quaternion();
        splats[i] = s;
    }
    return splats;
}

inline String _spz_fixture_path(const String &p_prefix) {
    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String base_temp = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : ".";
    return base_temp.path_join("godotgs_spz_fixture_" + p_prefix + "_" + itos(ticks) + ".spz");
}

} // namespace TestGaussianSplattingSPZ

TEST_CASE("[GaussianSplatting][SPZ] synthetic writer round-trips through SPZLoader") {
    using namespace TestGaussianSplattingSPZ;

    const String path = _spz_fixture_path("roundtrip");
    LocalVector<TestGaussianSplatting::SyntheticSpzSplat> splats = _make_spz_splats(4);

    REQUIRE_MESSAGE(TestGaussianSplatting::write_synthetic_spz(path, splats),
            "Synthetic SPZ writer should produce a file");

    SPZLoader loader;
    Error err = loader.load_file(path);
    CHECK_MESSAGE(err == OK, "SPZLoader should accept the synthetic SPZ file");

    Ref<GaussianData> data = loader.get_gaussian_data();
    REQUIRE_MESSAGE(data.is_valid(), "Loaded GaussianData should be valid");
    CHECK_EQ(data->get_count(), 4);

    if (data->get_count() == 4) {
        // Positions are fixed-point encoded with fractional_bits=12 and integer
        // inputs, so they round-trip exactly.
        for (int i = 0; i < 4; i++) {
            CHECK(Math::is_equal_approx(data->get_gaussian(i).position.x, float(i)));
        }
    }

    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ] importer default options are a no-op (count unchanged)") {
#ifndef TOOLS_ENABLED
    MESSAGE("Skipping - ResourceImporterSPZ requires TOOLS_ENABLED");
    return;
#else
    using namespace TestGaussianSplattingSPZ;

    const uint32_t kCount = 16;
    LocalVector<TestGaussianSplatting::SyntheticSpzSplat> splats = _make_spz_splats(kCount);

    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String source_path = "user://godotgs_spz_noop_" + itos(ticks) + ".spz";
    const String save_base_path = "user://godotgs_spz_noop_" + itos(ticks) + "_asset";

    REQUIRE_MESSAGE(TestGaussianSplatting::write_synthetic_spz(source_path, splats),
            "Should write synthetic SPZ fixture for the importer");

    Ref<ResourceImporterSPZ> importer;
    importer.instantiate();

    // Ultra + no prune options -> defaults (prune_ratio 1.0 / threshold 0.0) are
    // a strict no-op. max_splats=0 + density=1.0 keeps all splats in source order.
    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("quality/max_splats"), 0);
    options.insert(StringName("quality/density_multiplier"), 1.0);
    options.insert(StringName("processing/sort_by_opacity"), false);
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &metadata_variant);
    CHECK_MESSAGE(import_err == OK, "SPZ import should succeed at default (no-op) prune options");

    if (import_err == OK) {
        Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
        REQUIRE_MESSAGE(asset.is_valid(), "Imported GaussianSplatAsset should load from disk");
        CHECK_EQ(int(asset->get_splat_count()), int(kCount));

        Dictionary md = metadata_variant;
        CHECK_EQ(int(md.get(StringName("original_splat_count"), -1)), int(kCount));
        CHECK_EQ(int(md.get(StringName("pre_prune_splat_count"), -1)), int(kCount));
        CHECK_EQ(int(md.get(StringName("splat_count"), -1)), int(kCount));
    }

    DirAccess::remove_absolute(source_path);
    DirAccess::remove_absolute(save_base_path + ".res");
#endif // TOOLS_ENABLED
}

TEST_CASE("[GaussianSplatting][SPZ] importer prune_ratio 0.5 drops half and keeps highest-importance") {
#ifndef TOOLS_ENABLED
    MESSAGE("Skipping - ResourceImporterSPZ requires TOOLS_ENABLED");
    return;
#else
    using namespace TestGaussianSplattingSPZ;

    const uint32_t kCount = 16;
    const int kExpectedKept = 8; // round(16 * 0.5)
    LocalVector<TestGaussianSplatting::SyntheticSpzSplat> splats = _make_spz_splats(kCount);

    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String source_path = "user://godotgs_spz_prune_" + itos(ticks) + ".spz";
    const String save_base_path = "user://godotgs_spz_prune_" + itos(ticks) + "_asset";

    REQUIRE_MESSAGE(TestGaussianSplatting::write_synthetic_spz(source_path, splats),
            "Should write synthetic SPZ fixture for the prune importer test");

    Ref<ResourceImporterSPZ> importer;
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
    CHECK_MESSAGE(import_err == OK, "SPZ import with prune_ratio 0.5 should succeed");

    if (import_err == OK) {
        Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
        REQUIRE_MESSAGE(asset.is_valid(), "Pruned GaussianSplatAsset should load from disk");
        CHECK_EQ(int(asset->get_splat_count()), kExpectedKept);

        // Dual counts: original == 16, pre-prune == 16, final (splat_count) == 8.
        Dictionary md = metadata_variant;
        CHECK_EQ(int(md.get(StringName("original_splat_count"), -1)), int(kCount));
        CHECK_EQ(int(md.get(StringName("pre_prune_splat_count"), -1)), int(kCount));
        CHECK_EQ(int(md.get(StringName("splat_count"), -1)), kExpectedKept);

        // The kept splats must be the highest-importance suffix (indices 8..15),
        // identifiable by their distinct integer position.x.
        PackedFloat32Array positions = asset->get_positions();
        REQUIRE(positions.size() == kExpectedKept * 3);
        for (int j = 0; j < kExpectedKept; j++) {
            const float x = positions[j * 3 + 0];
            CHECK_MESSAGE(x >= 7.5f,
                    vformat("Kept splat %d has position.x=%f; expected a high-importance splat (x >= 8)", j, x));
        }

        // Chunk-bake consistency: the baked streaming-chunk records must describe
        // the PRUNED arrays (sum of chunk counts == pruned splat_count), not the
        // stale pre-prune 16.
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
    DirAccess::remove_absolute(save_base_path + ".res");
#endif // TOOLS_ENABLED
}
