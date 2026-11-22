#pragma once
#include "test_macros.h"
#include "../resources/gaussian_splat_region_override.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "../nodes/gaussian_splat_node_3d.h"
#include "test_b2_bake.h"
#include <cstddef>

TEST_CASE("[GaussianSplatting][HLOD][C] packed override ABI and float upload round trip") {
    CHECK(sizeof(RegionOverrideGPU) == 32);
    CHECK(offsetof(RegionOverrideGPU, emissive_opacity) == 16);
    CHECK(offsetof(RegionOverrideGPU, flags) == 24);
    CHECK(offsetof(RegionOverrideGPU, padding) == 28);
    CHECK(sizeof(InstanceDataGPU) == 128);
    CHECK(offsetof(InstanceDataGPU, lod) + sizeof(uint32_t) == 76);
    Ref<GaussianSplatRegionOverride> value; value.instantiate();
    for (float emission : {0.0f, 1.0f, 1000.0f, 65504.0f, 70000.0f}) {
        for (float opacity : {0.0f, 0.5f, 1.0f}) {
            value->set_emissive(Color(emission, emission, emission));
            value->set_opacity(opacity);
            const auto packed = value->pack_for_upload();
            const float expected = MIN(emission, 65504.0f);
            const float tolerance = MAX(expected / 1024.0f, 0.000001f);
            CHECK(Math::abs(Math::half_to_float(uint16_t(packed.emissive_opacity[0])) - expected) <= tolerance);
            CHECK(Math::abs(Math::half_to_float(uint16_t(packed.emissive_opacity[0] >> 16)) - expected) <= tolerance);
            CHECK(Math::abs(Math::half_to_float(uint16_t(packed.emissive_opacity[1])) - expected) <= tolerance);
            CHECK(Math::abs(Math::half_to_float(uint16_t(packed.emissive_opacity[1] >> 16)) - opacity) <= 0.0005f);
            CHECK(packed.padding == 0);
        }
    }
}

TEST_CASE("[GaussianSplatting][HLOD][C] region API resolves names again after a bake adds a volume") {
    using namespace TestB2Bake;
    LocalVector<Gaussian> g; g.push_back(splat(0.1));
    Array volumes; volumes.push_back(box("region", "root/Z", "canopy"));
    gs_hlod::SplatList baked; Dictionary metadata, report; String reason;
    if (gs_hlod::prepare_semantic_bake(input(g), volumes, Dictionary(), "", baked, metadata, report, reason) != OK) { FAIL("C manifest bake"); return; }
    struct Nodes {
        GaussianSplatWorld3D *world_node = memnew(GaussianSplatWorld3D);
        GaussianSplatNode3D *object_node = memnew(GaussianSplatNode3D);
        ~Nodes() { memdelete(object_node); memdelete(world_node); }
    } nodes;
    GaussianSplatWorld3D *world_node = nodes.world_node;
    GaussianSplatNode3D *object_node = nodes.object_node;
    CHECK(!world_node->is_inside_tree());
    CHECK(!object_node->is_inside_tree());
    Ref<GaussianSplatWorld> world; world.instantiate(); world->set_metadata(metadata); world_node->set_world(world);
    Ref<GaussianSplatAsset> asset; asset.instantiate(); asset->set_import_metadata(metadata); object_node->set_splat_asset(asset);
    int first = -1;
    for (Node *node : {static_cast<Node *>(world_node), static_cast<Node *>(object_node)}) {
        if (!node->has_method("get_region_id") || !node->has_method("get_region_names")) { FAIL("C public region lookup must be bound"); return; }
        const int id = node->call("get_region_id", StringName("canopy"));
        CHECK(id > 0); first = id;
        CHECK(int(node->call("get_region_id", StringName("missing"))) == -1);
        const PackedStringArray names = node->call("get_region_names");
        CHECK(names.has("canopy"));
    }
    volumes.push_back(box("region", "root/A", "extra"));
    if (gs_hlod::prepare_semantic_bake(input(g), volumes, Dictionary(), "", baked, metadata, report, reason) != OK) { FAIL("C second manifest bake"); return; }
    world->set_metadata(metadata); asset->set_import_metadata(metadata);
    for (Node *node : {static_cast<Node *>(world_node), static_cast<Node *>(object_node)}) {
        if (!node->has_method("get_region_id")) { FAIL("C public region lookup must remain bound after rebake"); return; }
        const int resolved = node->call("get_region_id", StringName("canopy"));
        CHECK(resolved > 0); CHECK(resolved != first);
        CHECK(resolved == GSRegionOverrideState::region_id(metadata, "canopy"));
    }
}

TEST_CASE("[GaussianSplatting][HLOD][C] whole asset defaults explicit regions and hide remain independent") {
    Ref<GSRegionOverrideState> state; state.instantiate();
    Dictionary region; region["id"] = 3; region["name"] = "leaves";
    Array regions; regions.push_back(region); Dictionary metadata; metadata["regions"] = regions;
    state->set_manifest(metadata);
    state->whole_asset()->set_emissive(Color(1, 0, 0));
    Ref<GaussianSplatRegionOverride> leaves; leaves.instantiate(); leaves->set_emissive(Color(0, 1, 0));
    state->set_override(3, leaves);
    LocalVector<RegionOverrideGPU> rows; state->snapshot(rows);
    if (rows.size() != 4) { FAIL("segment includes every manifest id"); return; }
    CHECK(Math::half_to_float(uint16_t(rows[2].emissive_opacity[0])) == 1);
    CHECK(Math::half_to_float(uint16_t(rows[3].emissive_opacity[0] >> 16)) == 1);
    state->set_hidden(true); state->snapshot(rows);
    for (const auto &row : rows) { CHECK((row.flags & GS_REGION_HIDE) != 0); }
    state->set_hidden(false); state->clear_override(3); state->snapshot(rows);
    CHECK((rows[3].flags & GS_REGION_HIDE) == 0);
    CHECK(Math::half_to_float(uint16_t(rows[3].emissive_opacity[0])) == 1);
}
