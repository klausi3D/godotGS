#pragma once

#include "test_macros.h"
#include "../io/spz_loader.h"
#include "../thirdparty/spz/load-spz.h"
#include "../io/resource_importer_spz.h"
#include "../io/streaming_chunk_bake.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "../io/ply_loader.h"
#include "synthetic_ply_writer.h"
#include "synthetic_spz_writer.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
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

// ---------------------------------------------------------------------------
// SPZ v2 rotation decode (#1154).
//
// The reference encoder (nianticlabs/spz load-spz.cc, packQuaternionFirstThree)
// normalises the quaternion, multiplies it by -127.5 when w < 0 (moving it into
// the w >= 0 hemisphere) or +127.5 otherwise, adds 127.5 and stores x, y, z as
// UNSIGNED bytes via toUint8 (round half away from zero, clamp to 0..255). The
// decode is (byte - 127.5) / 127.5 with w = sqrt(max(0, 1 - |xyz|^2)).
// ---------------------------------------------------------------------------
namespace TestGaussianSplattingSPZ {

// A quaternion and its sign-flipped twin are the same rotation, so compare
// through |dot|. 0.9995 allows the v2 quantisation step (1/127.5 per component;
// the worst case among the inputs below is 0.99997) and rejects the pre-#1154
// int8/127 decode by a wide margin (its best case among the inputs is 0.60).
inline bool _spz_same_rotation(const Quaternion &p_a, const Quaternion &p_b) {
    return Math::abs(p_a.normalized().dot(p_b.normalized())) >= 0.9995f;
}

} // namespace TestGaussianSplattingSPZ

TEST_CASE("[GaussianSplatting][SPZ] v2 rotation bytes decode with the reference 127.5 offset (#1154)") {
    using namespace TestGaussianSplattingSPZ;

    // Bytes pinned from the reference formula, independent of both the loader and
    // write_synthetic_spz, so this fails if either side drifts from the format.
    // Each row: source quaternion (x, y, z, w) -> the three bytes the reference
    // encoder writes for it, e.g. 0.301511 * 127.5 + 127.5 = 165.94 -> 166.
    struct PinnedRotation {
        Quaternion source;
        uint8_t bytes[3];
    };
    const PinnedRotation pinned[] = {
        { Quaternion(0.0f, 0.0f, 0.0f, 1.0f), { 128, 128, 128 } }, // identity: 127.5 rounds to 128
        { Quaternion(0.3f, -0.5f, 0.1f, 0.8f).normalized(), { 166, 63, 140 } },
        // w < 0: the encoder negates the quaternion before packing.
        { Quaternion(-0.6f, 0.2f, 0.4f, -0.66f).normalized(), { 204, 102, 76 } },
        { Quaternion(0.7071068f, 0.0f, 0.0f, 0.7071068f), { 218, 128, 128 } }, // 90 degrees about X
        { Quaternion(0.0f, 0.0f, 0.38268343f, 0.92387953f), { 128, 128, 176 } }, // 45 degrees about Z
    };
    const uint32_t count = sizeof(pinned) / sizeof(pinned[0]);

    // SoA payload: positions (9 B, zero), alphas (255), colours (128), scales
    // (160 -> exp(160 / 16 - 10) = 1), rotations (3 B).
    LocalVector<uint8_t> payload;
    for (uint32_t i = 0; i < count * 9; i++) {
        payload.push_back(0);
    }
    for (uint32_t i = 0; i < count; i++) {
        payload.push_back(255);
    }
    for (uint32_t i = 0; i < count * 3; i++) {
        payload.push_back(128);
    }
    for (uint32_t i = 0; i < count * 3; i++) {
        payload.push_back(160);
    }
    for (uint32_t i = 0; i < count; i++) {
        payload.push_back(pinned[i].bytes[0]);
        payload.push_back(pinned[i].bytes[1]);
        payload.push_back(pinned[i].bytes[2]);
    }
    REQUIRE(payload.size() == count * 19);

    const String path = _spz_fixture_path("v2_rotation_pinned");
    REQUIRE(TestGaussianSplatting::write_spz_v2_payload(path, count, payload));

    SPZLoader loader;
    REQUIRE(loader.load_file(path) == OK);
    Ref<GaussianData> data = loader.get_gaussian_data();
    if (data.is_null()) {
        FAIL("SPZLoader must produce GaussianData for a valid v2 file");
        DirAccess::remove_absolute(path);
        return;
    }
    if (data->get_count() != int(count)) {
        FAIL(vformat("SPZLoader loaded %d splats, expected %d", data->get_count(), int(count)));
        DirAccess::remove_absolute(path);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        const Quaternion loaded = data->get_gaussian(int(i)).rotation;
        CHECK_MESSAGE(_spz_same_rotation(loaded, pinned[i].source),
                vformat("splat %d: bytes (%d, %d, %d) decoded to %s, expected %s (sign-insensitive)",
                        int(i), int(pinned[i].bytes[0]), int(pinned[i].bytes[1]), int(pinned[i].bytes[2]), loaded, pinned[i].source));
    }

    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ] synthetic writer round-trips non-identity v2 rotations (#1154)") {
    using namespace TestGaussianSplattingSPZ;

    const Quaternion sources[] = {
        Quaternion(),
        Quaternion(Vector3(1, 0, 0), Math::deg_to_rad(90.0f)),
        Quaternion(Vector3(0, 1, 0), Math::deg_to_rad(-60.0f)),
        Quaternion(Vector3(0, 0, 1), Math::deg_to_rad(45.0f)),
        Quaternion(Vector3(1, 2, -3).normalized(), Math::deg_to_rad(130.0f)),
        Quaternion(-0.6f, 0.2f, 0.4f, -0.66f).normalized(), // w < 0
    };
    const uint32_t count = sizeof(sources) / sizeof(sources[0]);

    LocalVector<TestGaussianSplatting::SyntheticSpzSplat> splats = _make_spz_splats(count);
    for (uint32_t i = 0; i < count; i++) {
        splats[i].rotation = sources[i];
    }

    const String path = _spz_fixture_path("v2_rotation_roundtrip");
    REQUIRE(TestGaussianSplatting::write_synthetic_spz(path, splats));

    SPZLoader loader;
    REQUIRE(loader.load_file(path) == OK);
    Ref<GaussianData> data = loader.get_gaussian_data();
    if (data.is_null()) {
        FAIL("SPZLoader must produce GaussianData for a valid v2 file");
        DirAccess::remove_absolute(path);
        return;
    }
    if (data->get_count() != int(count)) {
        FAIL(vformat("SPZLoader loaded %d splats, expected %d", data->get_count(), int(count)));
        DirAccess::remove_absolute(path);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        const Quaternion loaded = data->get_gaussian(int(i)).rotation;
        CHECK_MESSAGE(_spz_same_rotation(loaded, sources[i]),
                vformat("splat %d: wrote %s, loaded %s (sign-insensitive)", int(i), sources[i], loaded));
    }

    DirAccess::remove_absolute(path);
}

// ---------------------------------------------------------------------------
// SPZ colour decode (#1056).
//
// The reference encoder (nianticlabs/spz splat-utils.h colorScale = 0.15;
// load-spz.cc packs toUint8(f_dc * 0.15 * 255 + 0.5 * 255)) stores the SH DC
// coefficient, and its display colour is 0.5 + SH_C0 * f_dc. The renderer's
// contract for a LINEAR_RGB-tagged splat is sh_dc = SH_C0 * f_dc, displayed as
// sh_dc + 0.5. Before #1056 SPZLoader stored byte / 255 under the same tag, so a
// mid-grey byte of 128 rendered at about 1.0.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][SPZ] colour bytes decode to the reference SH DC, byte 128 is mid-grey (#1056)") {
    using namespace TestGaussianSplattingSPZ;

    static constexpr float kShC0 = 0.28209479177387814f;
    // One splat per colour byte, the same byte on all three channels.
    const uint8_t colour_bytes[3] = { 128, 255, 0 };
    // byte 128: dc = 0 packs to 127.5, rounded to 128, so the decode is half a
    // byte step off zero: SH_C0 * (128 / 255 - 0.5) / 0.15 = 0.003688.
    // byte 255 / 0: +-SH_C0 * 0.5 / 0.15 = +-0.940316.
    const float expected_sh_dc[3] = { 0.003688f, kShC0 * (0.5f / 0.15f), -kShC0 * (0.5f / 0.15f) };
    const uint32_t count = 3;

    LocalVector<uint8_t> payload;
    for (uint32_t i = 0; i < count * 9; i++) {
        payload.push_back(0); // positions
    }
    for (uint32_t i = 0; i < count; i++) {
        payload.push_back(255); // alphas
    }
    for (uint32_t i = 0; i < count; i++) {
        payload.push_back(colour_bytes[i]);
        payload.push_back(colour_bytes[i]);
        payload.push_back(colour_bytes[i]);
    }
    for (uint32_t i = 0; i < count * 3; i++) {
        payload.push_back(160); // scales: exp(160 / 16 - 10) = 1
    }
    for (uint32_t i = 0; i < count * 3; i++) {
        payload.push_back(128); // identity rotation
    }
    REQUIRE(payload.size() == count * 19);

    const String path = _spz_fixture_path("dc_bytes");
    REQUIRE(TestGaussianSplatting::write_spz_v2_payload(path, count, payload));

    SPZLoader loader;
    REQUIRE(loader.load_file(path) == OK);
    Ref<GaussianData> data = loader.get_gaussian_data();
    if (data.is_null() || data->get_count() != int(count)) {
        FAIL("SPZLoader must produce the three fixture splats");
        DirAccess::remove_absolute(path);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        const Gaussian g = data->get_gaussian(int(i));
        CHECK(gaussian_get_dc_encoding(g.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
        for (int c = 0; c < 3; c++) {
            const float v = c == 0 ? g.sh_dc.r : (c == 1 ? g.sh_dc.g : g.sh_dc.b);
            CHECK_MESSAGE(Math::abs(v - expected_sh_dc[i]) < 1e-4f,
                    vformat("byte %d channel %d decoded to sh_dc %f, expected %f",
                            int(colour_bytes[i]), c, v, expected_sh_dc[i]));
        }
    }

    // The property users see: byte 128 displays as mid-grey (sh_dc + 0.5 within
    // one 8-bit step of 0.5). The pre-#1056 decode displayed it at 1.002.
    const Gaussian grey = data->get_gaussian(0);
    CHECK(Math::abs((grey.sh_dc.r + 0.5f) - 0.5f) < 1.0f / 255.0f);

    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ] same f_dc via SPZ and PLY loads to the same sh_dc (#1056)") {
    using namespace TestGaussianSplattingSPZ;

    static constexpr float kShC0 = 0.28209479177387814f;
    const float f_dc[4] = { 1.0f, -1.0f, 0.5f, -2.5f };
    const uint32_t count = 4;

    // SPZ: the official producer packs f_dc; the fixture takes the display colour
    // 0.5 + SH_C0 * f_dc and converts it back to f_dc before packing.
    LocalVector<TestGaussianSplatting::SyntheticSpzSplat> spz_splats = _make_spz_splats(count);
    // PLY: write_gaussian_ply stores sh_dc / SH_C0 as f_dc_*.
    LocalVector<Gaussian> ply_splats;
    ply_splats.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const float display = 0.5f + kShC0 * f_dc[i];
        spz_splats[i].color = Color(display, display, display, 1.0f);
        Gaussian g;
        g.position = Vector3(float(i), 0.0f, 0.0f);
        g.scale = Vector3(1.0f, 1.0f, 1.0f);
        g.rotation = Quaternion();
        g.sh_dc = Color(kShC0 * f_dc[i], kShC0 * f_dc[i], kShC0 * f_dc[i], 1.0f);
        g.normal = Vector3(0.0f, 0.0f, 1.0f);
        g.area = 1.0f;
        g.opacity = 0.9f;
        ply_splats[i] = g;
    }

    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String spz_path = "user://godotgs_spz_dc_twin_" + itos(ticks) + ".spz";
    const String ply_path = "user://godotgs_spz_dc_twin_" + itos(ticks) + ".ply";
    const String save_base_path = "user://godotgs_spz_dc_twin_" + itos(ticks) + "_asset";
    REQUIRE(TestGaussianSplatting::write_synthetic_spz(spz_path, spz_splats));
    REQUIRE(TestGaussianSplatting::write_gaussian_ply(ply_path, ply_splats, false, false));

    auto cleanup = [&]() {
        DirAccess::remove_absolute(spz_path);
        DirAccess::remove_absolute(ply_path);
        DirAccess::remove_absolute(ply_path.get_basename() + ".gsplatcache");
        DirAccess::remove_absolute(save_base_path + ".res");
    };

    SPZLoader spz_loader;
    REQUIRE(spz_loader.load_file(spz_path) == OK);
    Ref<GaussianData> spz_data = spz_loader.get_gaussian_data();
    PLYLoader ply_loader;
    REQUIRE(ply_loader.load_file(ply_path) == OK);
    Ref<GaussianData> ply_data = ply_loader.get_gaussian_data();
    if (spz_data.is_null() || ply_data.is_null() || spz_data->get_count() != int(count) ||
            ply_data->get_count() != int(count)) {
        FAIL("both loaders must produce the four fixture splats");
        cleanup();
        return;
    }

    // SPZ quantises f_dc to 1 / (0.15 * 255) = 0.0261; half a step in sh_dc
    // space is 0.0037. The pre-#1056 decode is off by 0.36 or more here.
    const float tolerance = 0.004f;
    for (uint32_t i = 0; i < count; i++) {
        const Gaussian s = spz_data->get_gaussian(int(i));
        const Gaussian p = ply_data->get_gaussian(int(i));
        CHECK(gaussian_get_dc_encoding(s.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
        CHECK(gaussian_get_dc_encoding(p.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
        CHECK_MESSAGE(Math::abs(s.sh_dc.r - p.sh_dc.r) < tolerance,
                vformat("f_dc %f: SPZ sh_dc %f, PLY sh_dc %f", f_dc[i], s.sh_dc.r, p.sh_dc.r));
        CHECK(Math::abs(s.sh_dc.g - p.sh_dc.g) < tolerance);
        CHECK(Math::abs(s.sh_dc.b - p.sh_dc.b) < tolerance);
    }

#ifdef TOOLS_ENABLED
    // The SPZ importer copies the loader's sh_dc into the asset and tags the
    // asset "linear_rgb", so the imported route must agree with the raw one.
    {
        Ref<ResourceImporterSPZ> importer;
        importer.instantiate();
        HashMap<StringName, Variant> options;
        options.insert(StringName("quality/preset"), String("ultra"));
        options.insert(StringName("quality/max_splats"), 0);
        options.insert(StringName("quality/density_multiplier"), 1.0);
        options.insert(StringName("processing/sort_by_opacity"), false);
        options.insert(StringName("preview/generate_thumbnail"), false);
        REQUIRE(importer->import(ResourceUID::INVALID_ID, spz_path, save_base_path, options,
                        nullptr, nullptr, nullptr) == OK);
        Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
        if (asset.is_null()) {
            FAIL("ResourceImporterSPZ must write a loadable GaussianSplatAsset");
            cleanup();
            return;
        }
        Ref<GaussianData> imported;
        imported.instantiate();
        REQUIRE(imported->populate_from_asset(asset) == OK);
        if (imported->get_count() != int(count)) {
            FAIL("the imported SPZ asset must materialize the four fixture splats");
            cleanup();
            return;
        }
        for (uint32_t i = 0; i < count; i++) {
            const Gaussian g = imported->get_gaussian(int(i));
            const Gaussian raw = spz_data->get_gaussian(int(i));
            CHECK(gaussian_get_dc_encoding(g.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
            CHECK(Math::abs(g.sh_dc.r - raw.sh_dc.r) < 1e-6f);
            CHECK(Math::abs(g.sh_dc.g - raw.sh_dc.g) < 1e-6f);
            CHECK(Math::abs(g.sh_dc.b - raw.sh_dc.b) < 1e-6f);
        }
    }
#else
    MESSAGE("SPZ importer route not compiled (needs TOOLS_ENABLED); the raw routes above still ran.");
#endif // TOOLS_ENABLED

    cleanup();
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

// Compiled only with TOOLS_ENABLED (the importer is editor-only): in other builds the case
// does not exist rather than passing as an environment skip.
#ifdef TOOLS_ENABLED
TEST_CASE("[GaussianSplatting][SPZ] importer max_splats at density 1.0 spans the whole file, not a prefix (#1155)") {
    using namespace TestGaussianSplattingSPZ;

    // Same shape as the PLY case: the second half of the file lies in a
    // distinct region, so a file-order prefix keeps none of it.
    const uint32_t kCount = 16;
    const int kCap = int(kCount / 2);
    LocalVector<TestGaussianSplatting::SyntheticSpzSplat> splats = _make_spz_splats(kCount);
    for (uint32_t i = 0; i < kCount; i++) {
        // Region A: x in [0, 8). Region B: x in [1000, 1008). Integers survive
        // the 12-bit fixed-point position encoding exactly.
        splats[i].position = Vector3(int(i) < kCap ? float(i) : 1000.0f + float(int(i) - kCap), 0.0f, 0.0f);
    }

    const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    const String source_path = "user://godotgs_spz_cap_stride_" + itos(ticks) + ".spz";
    const String save_base_path = "user://godotgs_spz_cap_stride_" + itos(ticks) + "_asset";
    REQUIRE(TestGaussianSplatting::write_synthetic_spz(source_path, splats));

    Ref<ResourceImporterSPZ> importer;
    importer.instantiate();
    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("quality/max_splats"), kCap);
    options.insert(StringName("quality/density_multiplier"), 1.0);
    options.insert(StringName("processing/sort_by_opacity"), false);
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    const Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &metadata_variant);
    CHECK_MESSAGE(import_err == OK, "SPZ import with max_splats = half should succeed");

    if (import_err == OK) {
        Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
        if (asset.is_null()) {
            FAIL("ResourceImporterSPZ must write a loadable GaussianSplatAsset");
        } else {
            CHECK_EQ(int(asset->get_splat_count()), kCap);
            const PackedFloat32Array positions = asset->get_positions();
            int in_a = 0;
            int in_b = 0;
            for (int i = 0; i + 2 < positions.size(); i += 3) {
                if (positions[i] < 500.0f) {
                    in_a++;
                } else {
                    in_b++;
                }
            }
            // Stride 2 keeps one representative per source pair: four from each
            // half. The pre-#1155 prefix kept 8 / 0.
            CHECK_MESSAGE(in_a == kCap / 2, vformat("region A kept %d splats, expected %d", in_a, kCap / 2));
            CHECK_MESSAGE(in_b == kCap / 2, vformat("region B (second half of the file) kept %d splats, expected %d", in_b, kCap / 2));

            const Dictionary md = metadata_variant;
            const AABB bounds = md.get(StringName("bounds"), AABB());
            CHECK_MESSAGE(bounds.position.x + bounds.size.x >= 1000.0f,
                    vformat("asset bounds end at x = %f; region B starts at 1000", bounds.position.x + bounds.size.x));
        }
    }

    DirAccess::remove_absolute(source_path);
    DirAccess::remove_absolute(save_base_path + ".res");
}
#endif // TOOLS_ENABLED

// ---------------------------------------------------------------------------
// Malformed-input corpus (G2, exit criterion; program ledger #458).
//
// The official v4 producer starts with an uncompressed container header:
//   magic[0..3] version[4..7] num_points[8..11] sh_degree[12]
//   fractional_bits[13] flags[14] reserved[15]
// (io/spz_loader.cpp parses these little-endian). Each case below writes a
// structurally VALID fixture (proven to load by the positive control), then
// overwrites exactly ONE field in place, so the matching io/spz_loader.cpp guard
// is the sole reason the load fails. These lock in the Phase A A2 hardening
// (fractional_bits shift) plus every other SPZ header guard, which previously had
// zero regression coverage. All guards already exist, so no case can abort — the
// corpus is a regression guard only (a reverted guard flips the expected error).
// ---------------------------------------------------------------------------
namespace TestGaussianSplattingSPZ {

// Overwrite one byte at p_offset in place. READ_WRITE opens r+ (no truncation).
inline bool _spz_patch_u8(const String &p_path, uint64_t p_offset, uint8_t p_value) {
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ_WRITE);
    if (f.is_null()) {
        return false;
    }
    f->seek(p_offset);
    f->store_8(p_value);
    f.unref();
    return true;
}

// Overwrite a little-endian uint32 at p_offset in place (matches the loader's
// byte assembly: hdr[0] | hdr[1] << 8 | ...).
inline bool _spz_patch_u32(const String &p_path, uint64_t p_offset, uint32_t p_value) {
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ_WRITE);
    if (f.is_null()) {
        return false;
    }
    f->seek(p_offset);
    f->store_32(p_value);
    f.unref();
    return true;
}

// Write a structurally valid 4-splat SPZ (fractional_bits = 12) and assert it
// loads. This positive control proves the fixture is byte-identical to a
// known-good file except the single field each test patches.
inline String _spz_valid_control(const String &p_prefix) {
    const String path = _spz_fixture_path(p_prefix);
    if (!TestGaussianSplatting::write_synthetic_spz(path, _make_spz_splats(4))) {
        return String();
    }
    SPZLoader control;
    REQUIRE_MESSAGE(control.load_file(path) == OK,
            "positive control: the unmodified SPZ fixture must load so the patched field is the sole failure cause");
    return path;
}

} // namespace TestGaussianSplattingSPZ

TEST_CASE("[GaussianSplatting][SPZ][MalformedCorpus] fractional_bits > 24 is rejected as corrupt (A2)") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_valid_control("frac_bits");
    REQUIRE_FALSE(path.is_empty());
    // byte 13 -> 25: > 24 is nonsensical for 24-bit fixed-point and 1 << bits is
    // shift UB at >= 31. Guard: io/spz_loader.cpp fractional_bits > 24.
    REQUIRE(_spz_patch_u8(path, 13, 25));
    SPZLoader loader;
    CHECK_EQ(loader.load_file(path), ERR_FILE_CORRUPT);
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][MalformedCorpus] bad magic is rejected as unrecognized") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_valid_control("bad_magic");
    REQUIRE_FALSE(path.is_empty());
    REQUIRE(_spz_patch_u32(path, 0, 0xDEADBEEFu)); // != SPZ_MAGIC (0x5053474E)
    SPZLoader loader;
    CHECK_EQ(loader.load_file(path), ERR_FILE_UNRECOGNIZED);
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][MalformedCorpus] unsupported version is rejected as unrecognized") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_valid_control("bad_version");
    REQUIRE_FALSE(path.is_empty());
    REQUIRE(_spz_patch_u32(path, 4, 99u)); // supported versions are 1 through 4
    SPZLoader loader;
    CHECK_EQ(loader.load_file(path), ERR_FILE_UNRECOGNIZED);
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][MalformedCorpus] sh_degree > 4 is rejected as corrupt") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_valid_control("bad_sh_degree");
    REQUIRE_FALSE(path.is_empty());
    REQUIRE(_spz_patch_u8(path, 12, 5)); // byte 12; max supported degree is 4
    SPZLoader loader;
    CHECK_EQ(loader.load_file(path), ERR_FILE_CORRUPT);
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][MalformedCorpus] zero point count is rejected as corrupt") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_valid_control("zero_points");
    REQUIRE_FALSE(path.is_empty());
    REQUIRE(_spz_patch_u32(path, 8, 0u)); // num_points == 0
    SPZLoader loader;
    CHECK_EQ(loader.load_file(path), ERR_FILE_CORRUPT);
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][MalformedCorpus] point count over the safety cap is rejected as corrupt") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_valid_control("over_cap_points");
    REQUIRE_FALSE(path.is_empty());
    // 0xFFFFFFFF > MAX_SPZ_POINTS (33,554,432); the count cap fires before any
    // payload sizing, so no over-allocation is attempted.
    REQUIRE(_spz_patch_u32(path, 8, 0xFFFFFFFFu));
    SPZLoader loader;
    CHECK_EQ(loader.load_file(path), ERR_FILE_CORRUPT);
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][MalformedCorpus] header shorter than 16 bytes is rejected as corrupt") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_fixture_path("short_header");
    {
        Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
        REQUIRE(f.is_valid());
        f->store_32(0x5053474Eu); // valid SPZ magic (first byte 0x4E, not GZIP 0x1F)
        f->store_32(2u); // valid version -> 8 bytes total, < sizeof(SPZHeader) == 16
        f.unref();
    }
    SPZLoader loader;
    CHECK_EQ(loader.load_file(path), ERR_FILE_CORRUPT);
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][ReferenceAdapter] official producers preserve SH0-4 activation and coordinates") {
    using namespace TestGaussianSplattingSPZ;
    auto splats = _make_spz_splats(3);
    for (uint32_t i = 0; i < splats.size(); i++) {
        auto &s = splats[i];
        s.position = Vector3(-1.25f + i, 0.75f, -2.5f);
        s.color = Color(0.2f, 0.65f, 0.8f, 1);
        s.opacity = i == 0 ? 0 : i == 1 ? 0.4f : 1;
        s.scale = Vector3(0.1f, 0.3f, 0.7f);
        s.rotation = Quaternion(Vector3(1, 2, 3).normalized(), 0.7f);
        for (uint32_t term = 0; term < 24; term++) {
            s.sh[term] = Vector3(float(int(term % 7) - 3) * 0.125f, float(int(term % 5) - 2) * 0.125f, float(int(term % 3) - 1) * 0.125f);
        }
    }
    for (uint32_t version = 2; version <= 4; version++) {
        for (uint8_t degree = 0; degree <= 4; degree++) {
            for (int coordinates : { -1, 1, 8, 14 }) {
                for (bool antialiased : { false, true }) {
                    const String path = _spz_fixture_path("official_reference");
                    if (!TestGaussianSplatting::write_synthetic_spz(path, splats, 12, version, degree, antialiased, coordinates)) {
                        FAIL("Official SPZ producer failed");
                        return;
                    }
                    const PackedByteArray producer_bytes = FileAccess::get_file_as_bytes(path);
                    spz::UnpackOptions reference_options;
                    reference_options.to = spz::CoordinateSystem::RUB;
                    const spz::GaussianCloud reference = spz::loadSpz(producer_bytes.ptr(), producer_bytes.size(), reference_options);
                    if (reference.numPoints != int(splats.size()) || reference.colors.size() != splats.size() * 3 || reference.sh.size() != splats.size() * ((degree + 1) * (degree + 1) - 1) * 3) {
                        FAIL("Official reference decoder failed its producer output");
                        return;
                    }
                    SPZLoader loader;
                    const Error loaded = loader.load_file(path);
                    DirAccess::remove_absolute(path);
                    CHECK_EQ(loaded, OK);
                    const Ref<GaussianData> data = loader.get_gaussian_data();
                    if (loaded != OK || data.is_null() || data->get_count() != int(splats.size())) {
                        FAIL("Producer file did not load completely");
                        return;
                    }
                    CHECK_EQ(data->get_sh_degree(), degree);
                    CHECK_EQ(data->get_sh_first_order_count(), degree == 0 ? 0 : 3);
                    CHECK_EQ(data->get_sh_high_order_count(), degree == 0 ? 0 : (degree + 1) * (degree + 1) - 4);
                    CHECK_EQ(data->get_antialiased(), antialiased);
                    for (uint32_t i = 0; i < splats.size(); i++) {
                        const Gaussian g = data->get_gaussian(i);
                        CHECK(g.position.is_equal_approx(splats[i].position));
                        CHECK(Math::abs(g.opacity - splats[i].opacity) < 0.002f);
                        CHECK(Math::abs(g.rotation.dot(splats[i].rotation)) > 0.999f);
                        CHECK_EQ(gaussian_get_dc_encoding(g.render_meta), GAUSSIAN_DC_ENCODING_LINEAR_RGB);
                        for (int axis = 0; axis < 3; axis++) {
                            CHECK(Math::abs(g.scale[axis] / splats[i].scale[axis] - 1) < 0.04f);
                            CHECK(Math::is_equal_approx(g.sh_dc[axis], 0.28209479177387814f * reference.colors[i * 3 + axis]));
                            CHECK(Math::abs(g.sh_dc[axis] + 0.5f - splats[i].color[axis]) < 0.004f);
                        }
                        for (uint32_t term = 0; term < data->get_sh_first_order_count(); term++) {
                            const uint32_t reference_index = (i * ((degree + 1) * (degree + 1) - 1) + term) * 3;
                            CHECK(g.sh_1[term].is_equal_approx(Vector3(reference.sh[reference_index], reference.sh[reference_index + 1], reference.sh[reference_index + 2])));
                            if (coordinates <= 8) {
                                CHECK(g.sh_1[term].is_equal_approx(splats[i].sh[term]));
                            }
                        }
                        const Vector3 *high = data->get_sh_high_order_coefficients_ptr();
                        if (data->get_sh_high_order_count() > 0 && !high) {
                            FAIL("Missing higher SH coefficients");
                            return;
                        }
                        for (uint32_t term = 0; term < data->get_sh_high_order_count(); term++) {
                            const Vector3 coefficient = high[i * data->get_sh_high_order_count() + term];
                            if (coordinates <= 8) {
                                CHECK(coefficient.is_equal_approx(splats[i].sh[term + 3]));
                            }
                            const uint32_t reference_index = (i * ((degree + 1) * (degree + 1) - 1) + term + 3) * 3;
                            CHECK(coefficient.is_equal_approx(Vector3(reference.sh[reference_index], reference.sh[reference_index + 1], reference.sh[reference_index + 2])));
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("[GaussianSplatting][SPZ][ReferenceAdapter] malformed v4 reload preserves the valid publication") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_fixture_path("v4_corruption");
    if (!TestGaussianSplatting::write_synthetic_spz(path, _make_spz_splats(4), 12, 4, 4, true)) {
        FAIL("Official producer failed");
        return;
    }
    const PackedByteArray original = FileAccess::get_file_as_bytes(path);
    if (original.size() < 128) {
        FAIL("Expected complete v4 producer output");
        return;
    }
    SPZLoader loader;
    if (loader.load_file(path) != OK) {
        FAIL("Positive control did not load");
        return;
    }
    const Ref<GaussianData> published = loader.get_gaussian_data();
    const Dictionary statistics = loader.get_load_statistics();
    const uint32_t toc = decode_uint32(original.ptr() + 16);
    for (int mutation = 0; mutation < 9; mutation++) {
        PackedByteArray bad = original;
        switch (mutation) {
            case 0: bad.set(20, 1); break; // Reserved header field.
            case 1: bad.set(15, 5); break; // Missing SH stream.
            case 2: encode_uint32(31, bad.ptrw() + 16); break; // TOC overlaps header.
            case 3: encode_uint64(UINT64_MAX, bad.ptrw() + toc); break;
            case 4: encode_uint64(1, bad.ptrw() + toc + 8); break; // Wrong expanded position extent.
            case 5: bad.set(14, 0x80); break; // Unsupported semantic flag.
            case 6: bad.set(14, 3); break; // Declared but absent extensions.
            case 7: bad.set(toc + 96, 0); break; // Broken first Zstd frame.
            case 8: bad.resize(bad.size() - 1); break;
        }
        Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
        if (file.is_null()) {
            FAIL("Cannot write corrupt fixture");
            return;
        }
        file->store_buffer(bad.ptr(), bad.size());
        file.unref();
        CHECK_NE(loader.load_file(path), OK);
        CHECK(loader.get_gaussian_data() == published);
        CHECK(loader.get_load_statistics() == statistics);
    }
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][ReferenceAdapter] imported SH4 antialias metadata survives resource cache") {
#ifdef TOOLS_ENABLED
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_fixture_path("import_sh4");
    const String destination = path + "_asset";
    auto splats = _make_spz_splats(3);
    splats[0].sh[23] = Vector3(-0.375f, 0.25f, 0.125f);
    if (!TestGaussianSplatting::write_synthetic_spz(path, splats, 12, 4, 4, true)) {
        FAIL("Official producer failed");
        return;
    }
    Ref<ResourceImporterSPZ> importer;
    importer.instantiate();
    HashMap<StringName, Variant> options;
    options.insert("quality/preset", String("ultra"));
    options.insert("processing/sort_by_opacity", false);
    options.insert("preview/generate_thumbnail", false);
    Variant metadata;
    const Error result = importer->import(ResourceUID::INVALID_ID, path, destination, options, nullptr, nullptr, &metadata);
    CHECK_EQ(result, OK);
    Ref<GaussianSplatAsset> asset = ResourceLoader::load(destination + ".res", "", ResourceFormatLoader::CACHE_MODE_IGNORE);
    if (result != OK || asset.is_null()) {
        FAIL("Imported resource did not round-trip");
        return;
    }
    Ref<GaussianData> data = asset->get_gaussian_data();
    if (data.is_null() || data->get_count() != 3 || data->get_sh_high_order_count() != 21) {
        FAIL("Cached resource lost full SH4");
        return;
    }
    CHECK(data->get_antialiased());
    CHECK(data->get_sh_high_order_coefficients_ptr()[20].is_equal_approx(splats[0].sh[23]));
    CHECK_EQ(importer->get_format_version(), 12);
    DirAccess::remove_absolute(path);
    DirAccess::remove_absolute(destination + ".res");
#endif
}

TEST_CASE("[GaussianSplatting][SPZ][ReferenceAdapter] legacy wrapper and gzip integrity remain bounded") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_fixture_path("legacy_wrapper");
    if (!TestGaussianSplatting::write_synthetic_spz(path, _make_spz_splats(4), 12, 2)) {
        FAIL("Official legacy producer failed");
        return;
    }
    const PackedByteArray gzip = FileAccess::get_file_as_bytes(path);
    if (gzip.size() < 18) {
        FAIL("Missing gzip producer output");
        return;
    }
    PackedByteArray decoded;
    decoded.resize(decode_uint32(gzip.ptr() + gzip.size() - 4));
    if (decoded.size() != 92 || Compression::decompress(decoded.ptrw(), decoded.size(), gzip.ptr(), gzip.size(), Compression::MODE_GZIP) != decoded.size()) {
        FAIL("Official legacy fixture expansion failed");
        return;
    }
    // Construct the explicitly non-Niantic historical GodotGS wrapper from a
    // genuine producer payload: raw 16-byte header, then gzip attributes.
    PackedByteArray wrapper;
    wrapper.resize(16 + Compression::get_max_compressed_buffer_size(decoded.size() - 16, Compression::MODE_GZIP));
    memcpy(wrapper.ptrw(), decoded.ptr(), 16);
    const int compressed = Compression::compress(wrapper.ptrw() + 16, decoded.ptr() + 16, decoded.size() - 16, Compression::MODE_GZIP);
    if (compressed <= 0) {
        FAIL("Could not construct compatibility wrapper");
        return;
    }
    wrapper.resize(16 + compressed);
    SPZLoader loader;
    for (int variant = 0; variant < 5; variant++) {
        PackedByteArray fixture = variant == 0 ? wrapper : gzip;
        if (variant == 2) {
            fixture.set(fixture.size() - 8, fixture[fixture.size() - 8] ^ 0x80); // CRC mismatch.
        } else if (variant == 3) {
            encode_uint32(UINT32_MAX, fixture.ptrw() + fixture.size() - 4); // Expansion cap.
        } else if (variant == 4) {
            encode_uint32(1, fixture.ptrw() + fixture.size() - 4); // Incorrect exact decoded size.
        }
        Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
        if (file.is_null()) {
            FAIL("Cannot write legacy fixture");
            return;
        }
        file->store_buffer(fixture.ptr(), fixture.size());
        file.unref();
        CHECK_EQ(loader.load_file(path), variant < 2 ? OK : ERR_FILE_CORRUPT);
        CHECK_EQ(loader.get_splat_count(), 4);
    }
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][ReferenceAdapter] v4 extensions reject ambiguous packing before decoding") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_fixture_path("extension_validation");
    if (!TestGaussianSplatting::write_synthetic_spz(path, _make_spz_splats(4), 12, 4, 4, false, 8)) {
        FAIL("Official extension producer failed");
        return;
    }
    const PackedByteArray original = FileAccess::get_file_as_bytes(path);
    if (original.size() < 140 || decode_uint32(original.ptr() + 16) != 44) {
        FAIL("Expected a coordinate extension from the official producer");
        return;
    }
    SPZLoader loader;
    if (loader.load_file(path) != OK) {
        FAIL("Extension positive control failed");
        return;
    }
    for (int mutation = 0; mutation < 5; mutation++) {
        PackedByteArray bad = original;
        if (mutation == 0) {
            encode_uint32(0x12345678, bad.ptrw() + 32); // Unknown extension.
        } else if (mutation == 1) {
            encode_uint32(5, bad.ptrw() + 36); // Invalid payload length.
        } else if (mutation == 2) {
            encode_uint32(17, bad.ptrw() + 40); // Unknown coordinates.
        } else if (mutation == 3) {
            bad.set(14, 0); // Extension flag absent.
        } else {
            bad.resize(original.size() + 12);
            memcpy(bad.ptrw(), original.ptr(), 44);
            memcpy(bad.ptrw() + 44, original.ptr() + 32, 12); // Duplicate descriptor.
            memcpy(bad.ptrw() + 56, original.ptr() + 44, original.size() - 44);
            encode_uint32(56, bad.ptrw() + 16);
        }
        Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
        if (file.is_null()) {
            FAIL("Cannot write extension fixture");
            return;
        }
        file->store_buffer(bad.ptr(), bad.size());
        file.unref();
        CHECK_EQ(loader.load_file(path), mutation == 0 ? ERR_UNAVAILABLE : ERR_FILE_CORRUPT);
        CHECK_EQ(loader.get_splat_count(), 4);
    }
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][ReferenceAdapter] legitimate repetitive producer output is accepted") {
    using namespace TestGaussianSplattingSPZ;
    LocalVector<TestGaussianSplatting::SyntheticSpzSplat> splats;
    splats.resize(100000);
    const String path = _spz_fixture_path("repetitive_producer");
    if (!TestGaussianSplatting::write_synthetic_spz(path, splats)) {
        FAIL("Official repetitive producer failed");
        return;
    }
    SPZLoader loader;
    CHECK_EQ(loader.load_file(path), OK);
    CHECK_EQ(loader.get_splat_count(), int(splats.size()));
    DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][SPZ][ReferenceAdapter] constructed legacy v1 Half positions remain readable") {
    using namespace TestGaussianSplattingSPZ;
    const String path = _spz_fixture_path("constructed_v1");
    auto splats = _make_spz_splats(1);
    splats[0].position = Vector3(1.25f, -0.75f, 2.5f);
    if (!TestGaussianSplatting::write_synthetic_spz(path, splats, 12, 2)) {
        FAIL("Official v2 producer failed");
        return;
    }
    const PackedByteArray gzip = FileAccess::get_file_as_bytes(path);
    PackedByteArray decoded;
    decoded.resize(35);
    if (Compression::decompress(decoded.ptrw(), decoded.size(), gzip.ptr(), gzip.size(), Compression::MODE_GZIP) != 35) {
        FAIL("Could not expand producer attributes");
        return;
    }
    // A constructed v1 envelope, explicitly not captured historical producer
    // output. The current Niantic writer no longer emits Half positions.
    PackedByteArray v1;
    v1.resize(32);
    memcpy(v1.ptrw(), decoded.ptr(), 16);
    encode_uint32(1, v1.ptrw() + 4);
    for (int axis = 0; axis < 3; axis++) {
        encode_uint16(Math::make_half_float(splats[0].position[axis]), v1.ptrw() + 16 + axis * 2);
    }
    memcpy(v1.ptrw() + 22, decoded.ptr() + 25, 10);
    PackedByteArray compressed;
    compressed.resize(Compression::get_max_compressed_buffer_size(v1.size(), Compression::MODE_GZIP));
    const int size = Compression::compress(compressed.ptrw(), v1.ptr(), v1.size(), Compression::MODE_GZIP);
    if (size <= 0) {
        FAIL("Could not compress constructed v1 envelope");
        return;
    }
    Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
    if (file.is_null()) {
        FAIL("Cannot write v1 fixture");
        return;
    }
    file->store_buffer(compressed.ptr(), size);
    file.unref();
    SPZLoader loader;
    const Error result = loader.load_file(path);
    CHECK_EQ(result, OK);
    if (result == OK && loader.get_splat_count() == 1) {
        CHECK(loader.get_gaussian_data()->get_gaussian(0).position.is_equal_approx(splats[0].position));
        CHECK_EQ(loader.get_header().version, 1);
    } else {
        FAIL("Constructed v1 payload did not load exactly one Gaussian");
    }
    DirAccess::remove_absolute(path);
}
