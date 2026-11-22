#pragma once

#include "../core/gs_project_settings.h"
#include "../core/gaussian_splat_bake_semantics.h"
#include "../io/ply_loader.h"
#include "../io/resource_importer_ply.h"
#include "../core/gaussian_splat_asset.h"
#include "test_hlod_d2_auto_tree.h"
#include "test_hlod_d2_tiled_bake.h"
#include "test_gaussian_layout_fingerprint.h"
#include "test_gs_world_writer_v4.h"
#include "core/object/class_db.h"
#include "core/config/project_settings.h"
#include "gs_b2_parent_merge.h"

namespace TestB2Bake {
// This Godot version has no Array::make; build small literal arrays explicitly.
template <typename... T>
inline Array array_of(T... p_values) {
    Array result;
    (result.push_back(Variant(p_values)), ...);
    return result;
}
inline Gaussian splat(float x, uint16_t region = 0, uint8_t wind = 0, bool valid = false) {
    Gaussian g;
    g.position = Vector3(x, 0.1, 0.1);
    g.scale = Vector3(0.01, 0.02, 0.03);
    g.rotation = Quaternion();
    g.opacity = 0.5f;
    g.semantic = gaussian_pack_semantic(region, wind, valid);
    return g;
}
inline gs_hlod::BakeInput input(const LocalVector<Gaussian> &g) {
    gs_hlod::BakeInput in;
    in.gaussians = g.ptr(); in.count = g.size();
    return in;
}
inline bool node_payload(const gs_hlod::BakeResult &p_baked, const GaussianSplatHlodNode &p_node, gs_hlod::SplatSpan &r_span) {
    // payload_first addresses [leaves | interiors]; the two test buffers are separate.
    const bool leaf = p_node.is_leaf();
    const uint64_t base = leaf ? 0 : p_baked.tree.leaf_splat_count;
    const auto &payload = leaf ? p_baked.leaf_gaussians : p_baked.tree.interior_gaussians;
    if (p_node.payload_first < base) { return false; }
    const uint64_t first = p_node.payload_first - base;
    if (first > payload.size() || p_node.payload_count == 0 || p_node.payload_count > payload.size() - first) { return false; }
    r_span = {payload.ptr() + first, nullptr, p_node.payload_count, 0};
    return true;
}
inline Dictionary box(const String &kind, const String &path, const String &name, float weight = 0) {
    Dictionary d;
    d["kind"] = kind; d["path"] = path; d["name"] = name;
    d["size"] = Vector3(2, 2, 2); d["transform"] = Transform3D(); d[gs::settings::WIND_WEIGHT_PROPERTY] = weight;
    return d;
}
inline String directory() {
    const String path = TestGaussianLayoutFingerprint::fixture_path("b2_islands", "");
    DirAccess::make_dir_recursive_absolute(path);
    return path;
}
inline String ply_header(const String &format, int count, const String &wind = "float") {
    return "ply\nformat " + format + " 1.0\nelement vertex " + itos(count) + "\nproperty float x\nproperty float y\nproperty float z\nproperty float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\nproperty float opacity\nproperty int region_id\nproperty " + wind + " " + String(gs::settings::WIND_WEIGHT_PROPERTY) + "\nproperty int island_id\nend_header\n";
}
}

TEST_CASE("[GaussianSplatting][HLOD][B2] legacy single-region bake keeps parent payload bytes") {
    using namespace TestB2Bake;
    LocalVector<Gaussian> g;
    for (int i = 0; i < 8; ++i) { g.push_back(splat(0.1)); g[i].sh_dc = Color(float(i) / 8, 0.25, 0.5); }
    gs_hlod::BakeParams params; params.leaf_max_splats = 4;
    gs_hlod::BakeResult baked; String reason;
    if (!gs_hlod::bake_world(input(g), params, baked, &reason) || baked.tree.nodes.size() != 3 || baked.tree.interior_gaussians.size() != 1) {
        FAIL("legacy fixture must have the parent's root and two index-split leaves"); return;
    }
    double center[3]; baked.tree.node_cell_center(baked.tree.nodes[0], center);
    // Two leaf payloads, concatenated and reframed exactly as the parent bake.
    LocalVector<Gaussian> reference;
    for (const auto &record : baked.leaf_gaussians) {
        Gaussian rel = record;
        rel.position -= Vector3(real_t(center[0]), real_t(center[1]), real_t(center[2]));
        reference.push_back(rel);
    }
    const uint32_t members[] = {0,1,2,3,4,5,6,7}; Gaussian parent;
    TestB2ParentMerge::merge_cell_sat({reference.ptr(), nullptr, 8, 0}, members, 8, parent, nullptr, nullptr);
    CHECK(std::memcmp(&parent, baked.tree.interior_gaussians.ptr(), sizeof(Gaussian)) == 0);
    for (uint32_t i = 0; i < g.size(); ++i) {
        CHECK(std::memcmp(&g[baked.leaf_source_index[i]], &baked.leaf_gaussians[i], sizeof(Gaussian)) == 0);
    }
    for (const auto &node : baked.tree.nodes) { CHECK(node.max_wind_weight == 0); CHECK((node.flags & (1u << 5)) == 0); }
    // The same cell also discriminates the new key on the parent: legacy byte
    // compatibility above must coexist with keeping two authored regions.
    for (uint32_t i = 0; i < g.size(); ++i) { g[i].semantic = gaussian_pack_semantic(1 + i % 2, 0, true); }
    gs_hlod::MergeScratch scratch;
    CHECK(gs_hlod::merged_count_at_eps({g.ptr(), nullptr, g.size(), 0}, Vector3(), 1, scratch) == 2);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][B2] rigid node provenance persists in V4 with other bits reserved") {
    using namespace TestGSWorldWriterV4;
    Paths paths; Fixture fixture;
    if (!paths.create() || !fixture.build()) { FAIL("V4 fixture"); return; }
    for (uint32_t i = 0; i < kNodes; ++i) {
        fixture.nodes[i].topology.flags |= 1u << 5;
        for (Gaussian &g : fixture.bodies[i].gaussians) { g.semantic = gaussian_pack_semantic(0, 0, true); }
    }
    Array regions; Dictionary region; region["id"] = 7; region["name"] = "door"; regions.push_back(region);
    fixture.spec.metadata["regions"] = regions; fixture.spec.metadata["islands"] = Array();
    WriterV4 writer;
    if (!begin(writer, paths, fixture) || !append_all(writer, fixture, paths)) { FAIL("write semantic V4"); return; }
    CHECK(writer.finalize(fixture.nodes) == OK);
    GaussianSplatHlodPagedSource source;
    if (!open_source(paths, source)) { return; }
    CHECK((source.get_nodes()[0].topology.flags & (1u << 5)) != 0);
    CHECK(source.get_nodes()[0].topology.max_wind_weight == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][B2] cell key preserves both regions and every baked level") {
    using namespace TestB2Bake;
    LocalVector<Gaussian> g;
    for (uint32_t i = 0; i < 16; ++i) { g.push_back(splat(0.1f, 1 + i % 2, 0, true)); }
    gs_hlod::SplatSpan span{g.ptr(), nullptr, g.size(), 0};
    gs_hlod::MergeScratch scratch;
    gs_hlod::SplatList out;
    CHECK(gs_hlod::merged_count_at_eps(span, Vector3(), 1, scratch) == 2);
    if (gs_hlod::merge_at_eps(span, Vector3(), 1, scratch, out) != OK || out.size() != 2) {
        FAIL("two regions in one cell must produce two splats"); return;
    }
    CHECK(gaussian_semantic_region(out.gaussians[0].semantic) == 1);
    CHECK(gaussian_semantic_region(out.gaussians[1].semantic) == 2);
    gs_hlod::BakeParams params; params.leaf_max_splats = 8; params.interior.reduction = 2;
    gs_hlod::BakeResult baked; String reason;
    if (!gs_hlod::bake_world(input(g), params, baked, &reason)) { FAIL(reason.utf8().get_data()); return; }
    for (const auto &node : baked.tree.nodes) {
        gs_hlod::SplatSpan payload;
        if (!node_payload(baked, node, payload)) { FAIL("node payload must address its leaf or interior buffer"); return; }
        uint32_t seen = 0;
        for (uint32_t i = 0; i < payload.count; ++i) {
            const uint32_t word = payload.gaussians[i].semantic;
            CHECK(gaussian_semantic_validate(word));
            const uint16_t region = gaussian_semantic_region(word);
            CHECK((region == 1 || region == 2));
            if (region < 32) { seen |= 1u << region; }
        }
        if (!node.is_leaf()) { CHECK(seen == 6); }
    }
}

TEST_CASE("[GaussianSplatting][HLOD][B2] semantic mean validity AND and summed class votes") {
    using namespace TestB2Bake;
    Gaussian g[3] = {splat(0.1, 7, 0, true), splat(0.1, 7, 200, true), splat(0.1, 7, 100, false)};
    g[0].opacity = 0.9; g[1].opacity = 0.4; g[2].opacity = 0.4;
    g[1].scale = Vector3(0.005, 0.03, 0.04); g[2].scale = Vector3(0.02, 0.03, 0.04);
    g[0].semantic |= 2u << 24; g[1].semantic |= 3u << 24; g[2].semantic |= 3u << 24;
    const uint32_t members[] = {0, 1, 2}; Gaussian out;
    gs_hlod::merge_cell_sat({g, nullptr, 3, 0}, members, 3, out, nullptr);
    CHECK(gaussian_semantic_region(out.semantic) == 7);
    // Area moment weights .00054, .00048, .00048: round(96) = 96.
    CHECK(gaussian_semantic_wind_weight(out.semantic) == 96);
    CHECK_FALSE(gaussian_semantic_valid(out.semantic));
    CHECK(((out.semantic >> 24) & 15) == 3);
}

TEST_CASE("[GaussianSplatting][HLOD][B2] node provenance and maximum describe own payload") {
    using namespace TestB2Bake;
    LocalVector<Gaussian> g;
    for (int i = 0; i < 16; ++i) { g.push_back(splat(0.1, 0, i == 0 ? 255 : 0, true)); }
    gs_hlod::BakeParams params; params.leaf_max_splats = 8;
    gs_hlod::BakeResult baked; String reason;
    if (!gs_hlod::bake_world(input(g), params, baked, &reason) || baked.tree.nodes.is_empty()) { FAIL("bake fixture"); return; }
    uint8_t subtree_max = 0;
    for (const auto &node : baked.tree.nodes) {
        gs_hlod::SplatSpan payload;
        if (!node_payload(baked, node, payload)) { FAIL("node payload must address its leaf or interior buffer"); return; }
        uint8_t maximum = 0; bool valid = true;
        for (uint32_t i = 0; i < payload.count; ++i) {
            const uint32_t word = payload.gaussians[i].semantic;
            maximum = MAX(maximum, gaussian_semantic_wind_weight(word)); valid &= gaussian_semantic_valid(word);
        }
        CHECK(node.max_wind_weight == maximum);
        CHECK(bool(node.flags & (1u << 5)) == valid);
        if (node.is_leaf()) { subtree_max = MAX(subtree_max, maximum); }
    }
    CHECK(baked.tree.nodes[0].max_wind_weight < subtree_max);
    // Rigid semantic content must remain distinguishable from legacy content.
    for (auto &record : g) { record.semantic = gaussian_pack_semantic(0, 0, true); }
    if (!gs_hlod::bake_world(input(g), params, baked, &reason)) { FAIL("rigid semantic bake"); return; }
    CHECK(baked.tree.nodes[0].max_wind_weight == 0);
    CHECK((baked.tree.nodes[0].flags & (1u << 5)) != 0);
    g[0].semantic = 0;
    if (!gs_hlod::bake_world(input(g), params, baked, &reason)) { FAIL("mixed legacy bake"); return; }
    CHECK((baked.tree.nodes[0].flags & (1u << 5)) == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][B2] F3 measures same-input interior bytes only when requested") {
    using namespace TestB2Bake;
    Gaussian g[8];
    for (int i = 0; i < 8; ++i) { g[i] = splat(0.1, 1 + i % 2, 0, true); }
    gs_hlod::SplatSpan span{g, nullptr, 8, 0};
    gs_hlod::InteriorBakeParams params;
    gs_hlod::InteriorBakeScratch scratch; gs_hlod::InteriorBakeResult result; String reason;
    if (!gs_hlod::bake_interior_node(&span, 1, Vector3(), 1, 0, params, scratch, result, &reason)) { FAIL("merge control"); return; }
    CHECK(result.single_region_interior_bytes == 0);
    params.measure_region_compression = true;
    if (!gs_hlod::bake_interior_node(&span, 1, Vector3(), 1, 0, params, scratch, result, &reason)) { FAIL("F3 merge"); return; }
    CHECK(result.region_keyed_interior_bytes == 2 * sizeof(Gaussian));
    CHECK(result.single_region_interior_bytes == sizeof(Gaussian));
}

TEST_CASE("[GaussianSplatting][HLOD][B2] volume precedence first match names and semantic rigid default") {
    using namespace TestB2Bake;
    LocalVector<Gaussian> g; g.push_back(splat(0.1, 23, 200, true));
    Dictionary source; source["fields"] = 3; // PLY region and wind properties
    Array volumes;
    volumes.push_back(box("region", "root/B_region", "later"));
    volumes.push_back(box("region", "root/A_region", "first"));
    volumes.push_back(box("wind", "root/A_wind", "", 0.25));
    volumes.push_back(box("wind", "root/B_wind", "", 1.0));
    gs_hlod::SplatList world; Dictionary metadata, report; String reason;
    if (gs_hlod::prepare_semantic_bake(input(g), volumes, source, "", world, metadata, report, reason) != OK || world.size() != 1) { FAIL("semantic preprocess"); return; }
    CHECK(world.gaussians[0].semantic == gaussian_pack_semantic(24, 64, true));
    const Array regions = metadata.get("regions", Array());
    CHECK(regions.size() == 3);
    const Dictionary counters = report.get("semantic_ply_overridden_by_volume", Dictionary());
    CHECK(int(counters.get("region", 0)) == 1);
    CHECK(int(counters.get("wind", 0)) == 1);
    volumes.resize(1); source.clear(); g[0].semantic = 0;
    if (gs_hlod::prepare_semantic_bake(input(g), volumes, source, "", world, metadata, report, reason) != OK || world.size() != 1) { FAIL("region-only preprocess"); return; }
    CHECK(gaussian_semantic_valid(world.gaussians[0].semantic));
    CHECK(gaussian_semantic_wind_weight(world.gaussians[0].semantic) == 0);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][B2] island extraction hole grid and metadata round trip") {
    using namespace TestB2Bake;
    for (bool covered : {false, true}) {
        LocalVector<Gaussian> g; g.push_back(splat(0.1));
        if (covered) { Gaussian ground = splat(0.1); ground.position.y = -2; g.push_back(ground); }
        Array volumes; volumes.push_back(box("island", "root/door", "door"));
        gs_hlod::SplatList world; Dictionary metadata, report; String reason;
        const String dir = directory();
        if (gs_hlod::prepare_semantic_bake(input(g), volumes, Dictionary(), dir, world, metadata, report, reason) != OK) { FAIL(reason.utf8().get_data()); return; }
        CHECK(world.size() == uint32_t(covered ? 1 : 0));
        const Array holes = report.get("islands", Array());
        const Array islands = metadata.get("islands", Array());
        if (holes.size() != 1 || islands.size() != 1) { FAIL("island metadata and hole report required"); return; }
        const Dictionary hole = holes[0], island = islands[0];
        CHECK(double(hole.get("hole_area_m2", -1)) == (covered ? 0 : 0.0625));
        CHECK(double(hole.get("island_xz_footprint_m2", -1)) == 0.0625);
        CHECK(double(hole.get("cell_size_m", -1)) == 0.25);
        CHECK(int(hole.get("extracted_splats", 0)) == 1);
        const String asset = island.get("asset_path", "");
        ResourceFormatLoaderGaussianSplatWorld loader; Error error = FAILED;
        Ref<GaussianSplatWorld> extracted = loader.load_resident(asset, &error);
        if (error != OK || extracted.is_null() || extracted->get_gaussian_data().is_null()) { FAIL("island asset must reopen"); return; }
        CHECK(extracted->get_splat_count() == 1);
        Ref<FileAccess> bytes = FileAccess::open(asset, FileAccess::READ);
        if (bytes.is_null()) { FAIL("island file"); return; }
        bytes->seek(TestGaussianLayoutFingerprint::WORLD_VERSION_OFFSET); CHECK(bytes->get_32() == 3); bytes.unref();
        // The plain metadata writer must retain both manifests.
        Ref<GaussianSplatWorld> holder = TestGaussianLayoutFingerprint::make_world(TestGaussianLayoutFingerprint::make_data());
        metadata["regions"] = TestB2Bake::array_of(Dictionary()); holder->set_metadata(metadata);
        const String path = dir.path_join("roundtrip.gsplatworld"); ResourceFormatSaverGaussianSplatWorld saver;
        if (saver.save(holder, path) != OK) { FAIL("manifest save"); return; }
        Ref<GaussianSplatWorld> restored = loader.load_resident(path, &error);
        if (restored.is_null()) { FAIL("manifest reopen"); return; }
        // The format persists JSON values: numbers reopen as FLOAT, packed arrays
        // reopen as Array, and floats use the writer's default precision.
        const Dictionary persisted_metadata = JSON::parse_string(JSON::stringify(metadata));
        CHECK(restored->get_metadata() == persisted_metadata);
        DirAccess::remove_absolute(path); DirAccess::remove_absolute(asset); DirAccess::remove_absolute(dir);
    }
}

TEST_CASE("[GaussianSplatting][WorldIO][B2] PLY integer ids and byte or float wind survive the loader") {
    using namespace TestB2Bake;
    for (bool byte : {false, true}) {
        const String path = TestGaussianLayoutFingerprint::fixture_path("b2", ".ply");
        Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
        if (file.is_null()) { FAIL("PLY fixture"); return; }
        file->store_string(String("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nproperty float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\nproperty float opacity\nproperty int region_id\nproperty ") + (byte ? "uchar" : "float") + " " + String(gs::settings::WIND_WEIGHT_PROPERTY) + "\nproperty int island_id\nend_header\n0 0 0 0 0 0 -3 -3 -3 1 0 0 0 0 17 " + (byte ? "128" : "0.5") + " 9\n");
        file.unref(); Ref<PLYLoader> loader; loader.instantiate();
        if (loader->load_file(path) != OK || loader->get_gaussian_data().is_null()) { FAIL("PLY load"); return; }
        Ref< ::GaussianData> data = loader->get_gaussian_data();
        CHECK(data->get_gaussian(0).semantic == gaussian_pack_semantic(17, 128, true));
        const Dictionary side = data->get_meta("gs_ply_semantics", Dictionary());
        const PackedInt32Array ids = side.get("island_ids", PackedInt32Array());
        if (ids.size() != 1) { FAIL("PLY island side data"); } else { CHECK(ids[0] == 9); }
        DirAccess::remove_absolute(path); DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
    }
}

TEST_CASE("[GaussianSplatting][WorldIO][B2] binary PLY and cache preserve semantics and island side data") {
    using namespace TestB2Bake;
    ProjectSettings *settings = ProjectSettings::get_singleton();
    if (!settings) { FAIL("project settings required to exercise the cache"); return; }
    const String cache_setting = "rendering/gaussian_splatting/import/use_gsplatworld_cache";
    struct RestoreSetting {
        ProjectSettings *settings; String key; bool present; Variant value;
        ~RestoreSetting() { if (present) { settings->set_setting(key, value); } else { settings->clear(key); } }
    } restore{settings, cache_setting, settings->has_setting(cache_setting), settings->get_setting(cache_setting, true)};
    settings->set_setting(cache_setting, true);
    const String path = TestGaussianLayoutFingerprint::fixture_path("b2_binary", ".ply");
    Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
    if (file.is_null()) { FAIL("binary PLY fixture"); return; }
    file->store_string(ply_header("binary_little_endian", 1, "uchar"));
    for (float value : {0.f,0.f,0.f,0.f,0.f,0.f,-3.f,-3.f,-3.f,1.f,0.f,0.f,0.f,0.f}) { file->store_float(value); }
    file->store_32(65535); file->store_8(173); file->store_32(16777217); file.unref();
    for (int pass = 0; pass < 2; ++pass) {
        PLYLoader loader;
        if (loader.load_file(path) != OK || loader.get_gaussian_data().is_null()) { FAIL("binary/cache PLY load"); return; }
        const Dictionary stats = loader.get_load_statistics();
        CHECK(stats.has("cache_hit")); CHECK(bool(stats.get("cache_hit", false)) == (pass == 1));
        CHECK(FileAccess::exists(path.get_basename() + ".gsplatcache"));
        const Ref< ::GaussianData> data = loader.get_gaussian_data();
        CHECK(data->get_gaussian(0).semantic == gaussian_pack_semantic(65535, 173, true));
        const Dictionary side = data->get_meta("gs_ply_semantics", Dictionary());
        const PackedInt32Array ids = side.get("island_ids", PackedInt32Array());
        if (ids.size() != 1) { FAIL("binary/cache island lane"); return; }
        CHECK(ids[0] == 16777217); // requires exact integer decoding, beyond float32 precision
    }
    DirAccess::remove_absolute(path); DirAccess::remove_absolute(path.get_basename() + ".gsplatcache");
}

#ifdef TOOLS_ENABLED
TEST_CASE("[GaussianSplatting][WorldIO][B2] packed PLY importer remaps semantic side rows with opacity sorting") {
    using namespace TestB2Bake;
    const String source = "user://b2_packed_semantics.ply", dest = "user://b2_packed_semantics_asset";
    Ref<FileAccess> file = FileAccess::open(source, FileAccess::WRITE);
    if (file.is_null()) { FAIL("import PLY fixture"); return; }
    file->store_string(ply_header("ascii", 2) + "0 0 0 0 0 0 -3 -3 -3 1 0 0 0 0 17 0.5 9\n1 0 0 0 0 0 -3 -3 -3 1 0 0 0 2 23 0.25 8\n"); file.unref();
    ResourceImporterPLY importer; HashMap<StringName, Variant> options;
    options.insert("quality/preset", "ultra"); options.insert("quality/max_splats", 0); options.insert("quality/density_multiplier", 1.0);
    options.insert("processing/sort_by_opacity", true); options.insert("preview/generate_thumbnail", false);
    Variant metadata;
    if (importer.import(ResourceUID::INVALID_ID, source, dest, options, nullptr, nullptr, &metadata) != OK) { FAIL("semantic PLY import"); return; }
    Ref<GaussianSplatAsset> asset = ResourceLoader::load(dest + ".res");
    if (asset.is_null() || asset->get_splat_count() != 2) { FAIL("packed semantic asset"); return; }
    Ref< ::GaussianData> data = asset->get_gaussian_data();
    if (data.is_null() || data->get_count() != 2) { FAIL("packed lane rebuild"); return; }
    CHECK(data->get_gaussian(0).semantic == gaussian_pack_semantic(23, 64, true));
    CHECK(data->get_gaussian(1).semantic == gaussian_pack_semantic(17, 128, true));
    const Dictionary side = data->get_meta("gs_ply_semantics", Dictionary());
    const PackedInt32Array ids = side.get("island_ids", PackedInt32Array());
    if (ids.size() != 2) { FAIL("packed island side lane"); return; }
    CHECK(ids[0] == 8); CHECK(ids[1] == 9);
    DirAccess::remove_absolute(source); DirAccess::remove_absolute(source.get_basename() + ".gsplatcache"); DirAccess::remove_absolute(dest + ".res");
}
#endif

TEST_CASE("[GaussianSplatting][HLOD][B2] convex hull and PLY island precedence use first volume and fail on region exhaustion") {
    using namespace TestB2Bake;
    LocalVector<Gaussian> g; g.push_back(splat(0.8));
    PackedVector3Array hull; hull.push_back(Vector3()); hull.push_back(Vector3(2,0,0)); hull.push_back(Vector3(0,2,0)); hull.push_back(Vector3(0,0,2));
    Dictionary region = box("region", "A", "hull"); region["size"] = Vector3(0.1,0.1,0.1); region["hull"] = hull;
    Array volumes; volumes.push_back(region);
    gs_hlod::SplatList world; Dictionary meta, report; String reason;
    if (gs_hlod::prepare_semantic_bake(input(g), volumes, Dictionary(), "", world, meta, report, reason) != OK || world.size() != 1) { FAIL("convex semantic bake"); return; }
    CHECK(world.gaussians[0].semantic == gaussian_pack_semantic(1, 0, true));
    g[0].semantic = gaussian_pack_semantic(65535, 0, true); Dictionary source; source["fields"] = 1;
    CHECK(gs_hlod::prepare_semantic_bake(input(g), volumes, source, "", world, meta, report, reason) == ERR_PARAMETER_RANGE_ERROR);
    CHECK(reason == "semantic_region_id_exhausted");
    g[0].semantic = gaussian_pack_semantic(0, 0, true); source["fields"] = 4;
    PackedInt32Array ids; ids.push_back(9); source["island_ids"] = ids;
    volumes.clear(); volumes.push_back(box("island", "A", "first")); volumes.push_back(box("island", "B", "later"));
    const String dir = directory();
    if (gs_hlod::prepare_semantic_bake(input(g), volumes, source, dir, world, meta, report, reason) != OK) { FAIL("island precedence"); return; }
    const Dictionary counts = report.get("semantic_ply_overridden_by_volume", Dictionary()); CHECK(int(counts.get("island", 0)) == 1);
    const Array islands = meta.get("islands", Array());
    if (islands.size() != 1) { FAIL("only the first matching island"); return; }
    const Dictionary island = islands[0]; CHECK(String(island.get("name", "")) == "first");
    DirAccess::remove_absolute(island.get("asset_path", "")); DirAccess::remove_absolute(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][B2] public bake extracts islands and persists rigid node provenance in v3") {
    using namespace TestB2Bake;
    Ref< ::GaussianData> data; data.instantiate(); Vector<Gaussian> records;
    records.push_back(splat(0.1)); records.push_back(splat(3)); data->set_gaussians(records);
    Ref<GaussianSplatWorld> world; world.instantiate(); world->set_gaussian_data(data);
    Array volumes; volumes.push_back(box("island", "A", "door")); volumes.push_back(box("region", "B", "world"));
    const String dir = directory(); Dictionary options; options["volumes"] = volumes; options["island_output_dir"] = dir;
    const Variant result = world->call("bake_hlod", options);
    if (result.get_type() != Variant::INT || int64_t(result) != OK) { FAIL("public semantic bake must succeed"); return; }
    CHECK(world->get_splat_count() == 1);
    const String path = dir.path_join("world.gsplatworld"); ResourceFormatSaverGaussianSplatWorld saver;
    if (saver.save(world, path) != OK) { FAIL("save semantic v3"); return; }
    ResourceFormatLoaderGaussianSplatWorld loader; Error error; Ref<GaussianSplatWorld> loaded = loader.load_resident(path, &error);
    if (loaded.is_null() || loaded->get_hlod_tree().nodes.is_empty()) { FAIL("reopen semantic v3 tree"); return; }
    CHECK((loaded->get_hlod_tree().nodes[0].flags & (1u << 5)) != 0);
    const Dictionary persisted_metadata = JSON::parse_string(JSON::stringify(world->get_metadata()));
    CHECK(loaded->get_metadata() == persisted_metadata);
    CHECK(loaded->get_gaussian_data()->get_gaussian(0).position.x == 3);
    const Array islands = world->get_metadata().get("islands", Array());
    for (const Variant &entry : islands) { const Dictionary island = entry; DirAccess::remove_absolute(island.get("asset_path", "")); }
    DirAccess::remove_absolute(path); DirAccess::remove_absolute(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][B2] malformed semantic metadata and duplicate island names fail closed") {
    using namespace TestB2Bake;
    LocalVector<Gaussian> g; g.push_back(splat(0.1));
    gs_hlod::SplatList world; Dictionary meta, report; String reason;
    for (const char *key : { "gs_ply_semantics", "regions", "islands" }) {
        Dictionary source; source[key] = 7;
        CHECK(gs_hlod::prepare_semantic_bake(input(g), Array(), source, "", world, meta, report, reason) == ERR_INVALID_DATA);
        CHECK_FALSE(reason.is_empty());
    }
    Dictionary existing; existing["name"] = "door"; existing["asset_path"] = "other/door.gsplatworld";
    Dictionary bounds; bounds["position"] = TestB2Bake::array_of(0,0,0); bounds["size"] = TestB2Bake::array_of(1,1,1);
    existing["bounds"] = bounds; existing["rest_transform"] = TestB2Bake::array_of(1,0,0,0,1,0,0,0,1,0,0,0);
    Dictionary source; source["islands"] = TestB2Bake::array_of(existing);
    Array volumes; volumes.push_back(box("island", "A", "door"));
    const String dir = directory();
    CHECK(gs_hlod::prepare_semantic_bake(input(g), volumes, source, dir, world, meta, report, reason) == ERR_INVALID_DATA);
    CHECK(reason == "semantic_duplicate_island_name");
    CHECK_FALSE(FileAccess::exists(dir.path_join("door.gsplatworld")));
    DirAccess::remove_absolute(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][B2] auto-tree filters islands before partition and forwards both manifests") {
    using namespace TestB2Bake;
    TestHlodD2AutoTree::Fixture fixture;
    if (!fixture.create(32768)) { FAIL("auto-tree semantic source"); return; }
    GsqAutoTreeSource input_source; String reason;
    if (input_source.open(fixture.input, &reason) != OK) { FAIL("open semantic source"); return; }
    gs_hlod::TiledBakeChunk chunk;
    if (input_source.read_chunk(0, chunk, &reason) != OK || chunk.gaussians.is_empty()) { FAIL("read semantic source"); return; }
    Dictionary region = box("region", "A", "world"); region["size"] = Vector3(10000,10000,10000);
    Dictionary island = box("island", "B", "prop"); Transform3D transform; transform.origin = chunk.gaussians[0].position; island["transform"] = transform; island["size"] = Vector3(0.25,0.25,0.25);
    Array volumes; volumes.push_back(region); volumes.push_back(island);
    Dictionary options = fixture.options(); options["volumes"] = volumes; options["measure_region_compression"] = true;
    const String output = fixture.dir.path_join("semantic.gsplatworld");
    const Dictionary report = GaussianSplatHlodAutoTree::build(fixture.input, output, options);
    if (int(report.get("error", FAILED)) != OK) { FAIL("auto-tree semantic bake: ", String(report.get("reason", "")).utf8().get_data()); return; }
    CHECK(report.has("region_keyed_interior_bytes")); CHECK(report.has("single_region_interior_bytes"));
    GaussianSplatHlodPagedSource source; GaussianSplatHlodTree tree; Dictionary meta;
    if (source.open(output, tree, meta, reason) != OK || tree.nodes.is_empty()) { FAIL("open semantic V4"); return; }
    const Array islands = meta.get("islands", Array()); const Array regions = meta.get("regions", Array());
    if (islands.size() != 1 || regions.size() != 1) { FAIL("semantic manifests"); return; }
    const Dictionary manifest = TestHlodD2AutoTree::json_file(gs_world_generator::manifest_path_for(output));
    CHECK(manifest.get("regions", Array()) == Variant(regions)); CHECK(manifest.get("islands", Array()) == Variant(islands));
    CHECK(tree.leaf_splat_count < input_source.get_info().splat_count);
    for (const auto &node : tree.nodes) { CHECK((node.flags & (1u << 5)) != 0); CHECK(node.max_wind_weight == 0); }
}

TEST_CASE("[GaussianSplatting][HLOD][B2] tiled bake through region and wind volumes keeps closed chunk bounds") {
    // GS-GAME-1 run 45026893667 follow-up: semantic_job() volumes on the T0 world
    // failed the tiled bake with error 30 (ERR_INVALID_DATA). One chunk whose first
    // centre is x = -170.6667 and whose last is x = 0.01: an AABB grown with
    // expand_to stores a float32 size whose get_end() is 0.0099945 < 0.01, so the
    // advertised closed bounds exclude a centre the chunk then returns.
    using namespace TestB2Bake;
    TestHlodD2TiledBake::Input in = TestHlodD2TiledBake::make_input(2048, 1);
    const uint32_t count = in.gaussians.size();
    for (uint32_t i = 0; i < count; ++i) {
        in.gaussians[i].position.x = i + 1 == count ? 0.01f : -170.6667f + 170.0f * float(i) / float(count);
    }
    {
        AABB grown(in.gaussians[0].position, Vector3());
        for (uint32_t i = 1; i < count; ++i) { grown.expand_to(in.gaussians[i].position); }
        if (!(grown.get_end().x < in.gaussians[count - 1].position.x)) { FAIL("fixture no longer exercises float32 AABB end rounding"); return; }
    }
    TestHlodD2TiledBake::Source plain(in, false);
    Dictionary region = box("region", "/near/region", "near");
    Transform3D band; band.origin = Vector3(-128, 3, 3); region["transform"] = band; region["size"] = Vector3(96, 16, 16);
    Dictionary wind = box("wind", "/near/wind", "near", 0.6f);
    wind["transform"] = band; wind["size"] = Vector3(96, 16, 16);
    Array volumes; volumes.push_back(region); volumes.push_back(wind);
    gs_hlod::SemanticBakeSource semantics;
    if (semantics.open(plain, volumes, Dictionary(), String()) != OK) { FAIL("semantic open: ", semantics.get_reason().utf8().get_data()); return; }
    // The TiledBakeSource contract: every returned centre lies inside the closed
    // centre_bounds [position, get_end()] the source advertised for that chunk.
    for (uint32_t c = 0; c < semantics.get_info().chunk_count; ++c) {
        gs_hlod::TiledBakeChunkInfo info; gs_hlod::TiledBakeChunk chunk;
        if (semantics.get_chunk_info(c, info, nullptr) != OK || semantics.read_chunk(c, chunk, nullptr) != OK) { FAIL("semantic chunk read"); return; }
        const Vector3 end = info.centre_bounds.get_end();
        bool inside = true;
        for (const Gaussian &g : chunk.gaussians) {
            for (int a = 0; a < 3; ++a) { inside = inside && g.position[a] >= info.centre_bounds.position[a] && g.position[a] <= end[a]; }
        }
        CHECK_MESSAGE(inside, "semantic chunk centre outside its advertised closed bounds");
    }
    gs_hlod::BakeResult result; gs_hlod::TiledBakeReport report; String reason;
    const Error error = gs_hlod::TiledAutoTree::build(semantics, TestHlodD2TiledBake::params(), result, report, &reason);
    if (error != OK) { FAIL("tiled bake through semantic volumes: ", reason); return; }
    CHECK(result.tree.leaf_splat_count == count);
    uint32_t swaying = 0, keyed = 0;
    for (uint32_t i = 0; i < result.leaf_gaussians.size(); ++i) {
        const uint32_t semantic = result.leaf_gaussians[i].semantic;
        swaying += gaussian_semantic_wind_weight(semantic) != 0 ? 1u : 0u;
        keyed += gaussian_semantic_region(semantic) != 0 ? 1u : 0u;
    }
    CHECK(swaying > 0); CHECK(swaying < count); CHECK(keyed == swaying);
}
