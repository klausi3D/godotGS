#pragma once

#include "tests/test_macros.h"
#include "../core/gaussian_semantic.h"
#include "../core/gs_project_settings.h"
#include "../io/gaussian_splat_world_io.h"
#include "../nodes/gaussian_splat_node_3d.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include <limits>

namespace TestDIslandNodes {
template <typename... T>
Array array_of(T... p_values) {
    Array result;
    (result.push_back(Variant(p_values)), ...);
    return result;
}

struct Fixture {
    String directory = OS::get_singleton()->get_temp_path().path_join("gs_d_islands_" + itos(OS::get_singleton()->get_ticks_usec()));
    String world_path = directory.path_join("world.gsplatworld");
    Ref<GaussianSplatWorld> world;
    Array islands;
    Node3D *parent = memnew(Node3D);
    GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);

    Fixture() {
        node->set_auto_apply_on_ready(false);
        parent->add_child(node);
    }
    ~Fixture() {
        memdelete(parent); // Frees every created island node, including on a failed precondition.
        for (const char *name : { "door.gsplatworld", "gate.gsplatworld", "world.gsplatworld", "corrupt.gsplatworld" }) {
            DirAccess::remove_absolute(directory.path_join(name));
        }
        DirAccess::remove_absolute(directory);
    }
    Error bake() {
        using namespace GaussianSplatting;
        Ref<::GaussianData> data;
        data.instantiate();
        Vector<Gaussian> records;
        for (real_t x : { real_t(0), real_t(4), real_t(10) }) {
            Gaussian g;
            g.position = Vector3(x, 0, 0);
            g.scale = Vector3(0.01, 0.02, 0.03);
            g.rotation = Quaternion();
            g.opacity = 0.5f;
            records.push_back(g);
        }
        data->set_gaussians(records);
        world.instantiate();
        world->set_gaussian_data(data);
        Array volumes;
        for (int i = 0; i < 2; ++i) {
            Dictionary volume;
            volume["kind"] = "island";
            volume["path"] = i == 0 ? "A" : "B";
            volume["name"] = i == 0 ? "door" : "gate";
            volume["size"] = Vector3(1, 1, 1);
            volume["transform"] = Transform3D(Basis(), Vector3(i * 4, 0, 0));
            volumes.push_back(volume);
        }
        Dictionary wind;
        wind["kind"] = "wind";
        wind["path"] = "wind";
        wind["name"] = "wind";
        wind["size"] = Vector3(100, 100, 100);
        wind["transform"] = Transform3D();
        wind[gs::settings::WIND_WEIGHT_PROPERTY] = 0.5;
        volumes.push_back(wind);
        Dictionary options;
        options["volumes"] = volumes;
        options["island_output_dir"] = directory;
        const Error error = world->bake_hlod(options); // Real B2 producer, not an invented manifest fixture.
        if (error != OK) { return error; }
        islands = world->get_metadata().get("islands", Array());
        if (islands.size() != 2) { return ERR_INVALID_DATA; }
        // B2 currently emits identity. Exercise its documented row-major encoding with
        // non-identity rest placements too, then round-trip the actual producer metadata.
        Dictionary door = islands[0];
        door["rest_transform"] = array_of(0,-1,0, 1,0,0, 0,0,1, 2,3,5);
        Dictionary gate = islands[1];
        gate["rest_transform"] = array_of(2,0,0, 0,3,0, 0,0,4, -2,1,7);
        publish(islands);
        ResourceFormatSaverGaussianSplatWorld saver;
        const Error saved = saver.save(world, world_path);
        if (saved != OK) { return saved; }
        ResourceFormatLoaderGaussianSplatWorld loader;
        Error loaded_error = OK;
        world = loader.load_resident(world_path, &loaded_error);
        if (world.is_null() || loaded_error != OK) { return ERR_INVALID_DATA; }
        world->set_path(world_path);
        islands = world->get_metadata().get("islands", Array());
        if (islands.size() != 2) { return ERR_INVALID_DATA; }
        node->set_world(world);
        return OK;
    }
    void publish(const Variant &p_islands) {
        Dictionary metadata = world->get_metadata().duplicate(true);
        metadata["islands"] = p_islands;
        world->set_metadata(metadata);
    }
    bool has_reason(const String &p_name, const String &p_reason) const {
        for (const String &error : node->get_island_errors()) {
            if (error.contains(p_name) && error.contains(p_reason)) { return true; }
        }
        return false;
    }
};
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][D] island helper places B2 v3 assets and is idempotent") {
    using namespace TestDIslandNodes;
    SceneTree *tree = SceneTree::get_singleton();
    if (!tree || !tree->get_root()) { FAIL("island placement requires the SceneTree root"); return; }
    Fixture fixture;
    if (fixture.bake() != OK) { FAIL("tiny B2 world with two islands must bake and reopen"); return; }
    tree->get_root()->add_child(fixture.parent);
    const PackedStringArray names = fixture.node->call("get_island_names");
    PackedStringArray expected_names;
    expected_names.push_back("door");
    expected_names.push_back("gate");
    CHECK(names == expected_names);
    const Array created = fixture.node->call("create_island_nodes");
    // The inert seam on the parent fails here by assertion, before indexing any result.
    if (created.size() != 2) { FAIL("expected two island nodes, got ", created.size()); return; }
    const Transform3D expected[] = {
        Transform3D(Basis(0,-1,0, 1,0,0, 0,0,1), Vector3(2,3,5)),
        Transform3D(Basis(2,0,0, 0,3,0, 0,0,4), Vector3(-2,1,7))
    };
    for (int i = 0; i < 2; ++i) {
        GaussianSplatNode3D *island = Object::cast_to<GaussianSplatNode3D>(created[i]);
        if (!island) { FAIL("helper must return GaussianSplatNode3D objects"); return; }
        CHECK(island->get_parent() == fixture.node);
        CHECK(island->is_inside_tree());
        CHECK(String(island->get_name()) == (i == 0 ? "door" : "gate"));
        CHECK(island->get_transform() == expected[i]);
        const Ref<GaussianSplatAsset> asset = island->get_splat_asset();
        if (asset.is_null()) { FAIL("island asset must be assigned"); return; }
        const Ref<::GaussianData> data = asset->get_gaussian_data();
        if (data.is_null() || data->get_count() != 1) { FAIL("v3 island payload must be loaded"); return; }
        CHECK(data->get_gaussian(0).position == Vector3(i * 4, 0, 0));
        CHECK(data->get_gaussian(0).semantic == gaussian_pack_semantic(0, 128, true));
    }
    CHECK(fixture.node->get_island_errors().is_empty());
    CHECK(fixture.node->create_island_nodes().is_empty());
    CHECK(fixture.node->get_child_count() == 2);
    CHECK(fixture.node->get_island_errors().is_empty());
    // An explicitly supplied parent gets the same local rest transforms.
    Node3D *other = memnew(Node3D);
    fixture.parent->add_child(other);
    CHECK(fixture.node->create_island_nodes(other).size() == 2);
    CHECK(other->get_child_count() == 2);
    CHECK(fixture.node->create_island_nodes(other).is_empty());
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][D] island helper rejects missing unreadable malformed and duplicate entries") {
    using namespace TestDIslandNodes;
    Fixture fixture;
    if (fixture.bake() != OK) { FAIL("two-island B2 fixture"); return; }
    Dictionary door = fixture.islands[0];
    int expected_children = 0;
    SUBCASE("missing asset") {
        door["asset_path"] = fixture.directory.path_join("missing.gsplatworld");
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("door", "island_asset_missing"));
    }
    SUBCASE("unreadable asset") {
        const String path = fixture.directory.path_join("corrupt.gsplatworld");
        Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
        if (file.is_null()) { FAIL("write corrupt asset fixture"); return; }
        file->store_string("invalid island bytes");
        file.unref();
        door["asset_path"] = path;
        fixture.publish(array_of(door));
        ERR_PRINT_OFF;
        const Array created = fixture.node->create_island_nodes();
        ERR_PRINT_ON;
        CHECK(created.is_empty());
        CHECK(fixture.has_reason("door", "island_asset_unreadable"));
    }
    SUBCASE("duplicate name rejects every occurrence") {
        fixture.publish(array_of(door, door));
        CHECK(fixture.node->get_island_names().is_empty());
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.node->get_island_errors().size() == 2);
        CHECK(fixture.has_reason("door", "island_duplicate_name"));
    }
    SUBCASE("malformed transform does not prevent a valid sibling") {
        expected_children = 1;
        door["rest_transform"] = array_of(1, 2, 3);
        fixture.publish(array_of(door, fixture.islands[1]));
        CHECK(fixture.node->create_island_nodes().size() == 1);
        CHECK(fixture.has_reason("door", "island_rest_transform"));
        if (fixture.node->get_child_count() != 1) { FAIL("only the valid gate may be created"); return; }
        CHECK(fixture.node->get_child(0)->get_name() == StringName("gate"));
    }
    SUBCASE("malformed bounds") {
        door["bounds"] = "bad bounds";
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("door", "island_bounds"));
    }
    SUBCASE("wrongly typed transform number") {
        Array rest = door["rest_transform"];
        rest[0] = "1";
        door["rest_transform"] = rest;
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("door", "island_rest_transform"));
    }
    SUBCASE("nonfinite transform number") {
        Array rest = door["rest_transform"];
        rest[11] = std::numeric_limits<double>::infinity();
        door["rest_transform"] = rest;
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("door", "island_rest_transform"));
    }
    SUBCASE("missing asset path") {
        door.erase("asset_path");
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("door", "island_asset_path"));
    }
    SUBCASE("invalid node name") {
        door["name"] = "door/child";
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("door/child", "island_name"));
    }
    SUBCASE("non-dictionary entry") {
        fixture.publish(array_of(42));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("[0]", "island_entry_type"));
    }
    SUBCASE("non-array manifest") {
        fixture.publish(42);
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("islands", "island_manifest_type"));
    }
    SUBCASE("no world") {
        fixture.node->set_world(Ref<GaussianSplatWorld>());
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("world", "island_world_missing"));
    }
    SUBCASE("existing child of any type remains untouched") {
        expected_children = 1;
        Node *existing = memnew(Node);
        existing->set_name("door");
        fixture.node->add_child(existing);
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.node->get_child(0) == existing);
        CHECK(fixture.node->get_island_errors().is_empty());
    }
    CHECK(fixture.node->get_child_count() == expected_children);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][D] island helper resolves world-relative paths and refuses traversal") {
    using namespace TestDIslandNodes;
    Fixture fixture;
    if (fixture.bake() != OK) { FAIL("two-island B2 fixture"); return; }
    Dictionary door = fixture.islands[0];
    SUBCASE("relative path uses world file directory") {
        door["asset_path"] = "door.gsplatworld";
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().size() == 1);
        CHECK(fixture.node->get_island_errors().is_empty());
    }
    SUBCASE("escape is refused even when target exists") {
        door["asset_path"] = "../" + fixture.directory.get_file() + "/door.gsplatworld";
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("door", "island_asset_path_escape"));
    }
    SUBCASE("relative path needs world file location") {
        fixture.world->set_path("");
        door["asset_path"] = "door.gsplatworld";
        fixture.publish(array_of(door));
        CHECK(fixture.node->create_island_nodes().is_empty());
        CHECK(fixture.has_reason("door", "island_world_path_missing"));
    }
}
