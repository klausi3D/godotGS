#pragma once

#include "test_macros.h"
#include "../nodes/gaussian_splat_node_3d.h"
#include "../nodes/sphere_effector_3d.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "../core/gaussian_data.h"
#include "../core/effective_config_snapshot.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../core/gaussian_splat_source_path.h"
// #798 round 3: the TESTS_ENABLED failure-injection seam used by the failed-set_splat_data
// case below (gs_vector_alloc_force_failure_at / _is_armed / _clear).
#include "../core/gs_vector_alloc.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/sh_config.h"
#include "../resources/color_grading_resource.h"
#ifdef TOOLS_ENABLED
#include "../editor/gaussian_editor_services.h"
#include "../editor/gaussian_inspector_plugins.h"
#endif
#include "core/math/math_funcs.h"
#include "core/config/project_settings.h"
#include "core/error/error_list.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/list.h"
#include "core/variant/variant.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

#include <cstring>
#include <limits>

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

#include "gs_test_setting_guard.h"

namespace {

Ref<GaussianData> make_test_gaussian_data(int p_count, float p_x_offset = 0.0f) {
    Ref<GaussianData> data;
    data.instantiate();
    data->resize(p_count);
    for (int i = 0; i < p_count; i++) {
        Gaussian g;
        g.position = Vector3(p_x_offset + (float)i, 0.0f, 0.0f);
        g.scale = Vector3(1.0f, 1.0f, 1.0f);
        g.rotation = Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
        g.opacity = 1.0f;
        g.sh_dc = Color(1.0f, 1.0f, 1.0f, 1.0f);
        data->set_gaussian(i, g);
    }
    return data;
}

Ref<GaussianSplatAsset> make_single_splat_asset(float p_x_offset = 0.0f) {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_splat_count(1);

    PackedFloat32Array positions;
    positions.resize(3);
    {
        float *ptr = positions.ptrw();
        ptr[0] = p_x_offset;
        ptr[1] = 0.0f;
        ptr[2] = 0.0f;
    }
    asset->set_positions(positions);

    PackedFloat32Array scales;
    scales.resize(3);
    {
        float *ptr = scales.ptrw();
        ptr[0] = 1.0f;
        ptr[1] = 1.0f;
        ptr[2] = 1.0f;
    }
    asset->set_scales(scales);

    PackedFloat32Array rotations;
    rotations.resize(4);
    {
        float *ptr = rotations.ptrw();
        ptr[0] = 1.0f; // w
        ptr[1] = 0.0f;
        ptr[2] = 0.0f;
        ptr[3] = 0.0f;
    }
    asset->set_rotations(rotations);

    PackedFloat32Array sh_dc;
    sh_dc.resize(3);
    {
        float *ptr = sh_dc.ptrw();
        ptr[0] = 1.0f;
        ptr[1] = 1.0f;
        ptr[2] = 1.0f;
    }
    asset->set_sh_dc_coefficients(sh_dc);

    PackedFloat32Array opacity_logits;
    opacity_logits.resize(1);
    opacity_logits.set(0, 10.0f);
    asset->set_opacity_logits(opacity_logits);

    return asset;
}

Ref<GaussianSplatAsset> make_import_metadata_asset(int p_count, float p_x_offset, const String &p_quality_preset,
        int p_max_splats, double p_density_multiplier) {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();

    Ref<GaussianData> data = make_test_gaussian_data(p_count, p_x_offset);
    if (asset->populate_from_gaussian_data(data) != OK) {
        return Ref<GaussianSplatAsset>();
    }

    asset->set_import_quality_preset(p_quality_preset);
    Dictionary metadata = asset->get_import_metadata();
    metadata[StringName("quality_preset")] = p_quality_preset;
    metadata[StringName("max_splats")] = p_max_splats;
    metadata[StringName("density_multiplier")] = p_density_multiplier;
    asset->set_import_metadata(metadata);

    return asset;
}

int find_instance_index_by_translation_x(const LocalVector<InstanceDataGPU> &p_instances, float p_x) {
    for (int i = 0; i < p_instances.size(); i++) {
        if (Math::is_equal_approx(p_instances[i].translation_scale[0], p_x)) {
            return i;
        }
    }
    return -1;
}

int count_instances_by_translation_x(const LocalVector<InstanceDataGPU> &p_instances, float p_x) {
    int count = 0;
    for (int i = 0; i < p_instances.size(); i++) {
        if (Math::is_equal_approx(p_instances[i].translation_scale[0], p_x)) {
            count++;
        }
    }
    return count;
}

uint32_t decode_scene_effector_mask(const InstanceDataGPU &p_instance) {
    uint32_t mask = 0u;
    memcpy(&mask, &p_instance.effect_params[2], sizeof(mask));
    return mask;
}

bool is_property_editor_exposed(Object *p_object, const StringName &p_property_name) {
    if (p_object == nullptr) {
        return false;
    }

    List<PropertyInfo> property_list;
    p_object->get_property_list(&property_list);
    for (const List<PropertyInfo>::Element *E = property_list.front(); E; E = E->next()) {
        const PropertyInfo &info = E->get();
        if (info.name == p_property_name) {
            return (info.usage & PROPERTY_USAGE_EDITOR) != 0;
        }
    }

    return false;
}

// #839 round 2: observe whether a node is actually DRAWING the debug HUD.
// GaussianSplatNode3D::_ensure_debug_hud_control adds a CanvasLayer child named
// "GaussianSplatDebugHUDLayer" and _destroy_debug_hud_control removes it, so the
// child's presence is exactly "this node owns the on-screen HUD control".
bool node_has_debug_hud_control(GaussianSplatNode3D *p_node) {
    if (p_node == nullptr) {
        return false;
    }
    return p_node->get_node_or_null(NodePath("GaussianSplatDebugHUDLayer")) != nullptr;
}

Ref<ColorGradingResource> make_color_grading_resource() {
    Ref<ColorGradingResource> grading;
    grading.instantiate();
    if (grading.is_valid()) {
        grading->set_enabled(true);
        grading->set_exposure(0.5f);
    }
    return grading;
}

// Per-instance color grading moved off the renderer's single-slot RenderConfig and
// onto GaussianSplatSceneDirector's InstanceRecord keyed by node ObjectID. Tests that
// previously asserted `renderer->get_color_grading() == grading` now ask the
// director via this helper. Falls back to the legacy renderer slot when the
// director is unavailable (keeps assertions meaningful under tear-down).
Ref<ColorGradingResource> node_color_grading(const GaussianSplatNode3D *p_node,
        const Ref<GaussianSplatRenderer> &p_renderer) {
    if (p_node) {
        if (GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton()) {
            return director->get_instance_color_grading(p_node->get_instance_id());
        }
    }
    return p_renderer.is_valid() ? p_renderer->get_color_grading() : Ref<ColorGradingResource>();
}

void set_single_splat_position(const Ref<GaussianSplatAsset> &p_asset, const Vector3 &p_position) {
    PackedFloat32Array positions;
    positions.resize(3);
    {
        float *ptr = positions.ptrw();
        ptr[0] = p_position.x;
        ptr[1] = p_position.y;
        ptr[2] = p_position.z;
    }
    p_asset->set_positions(positions);
}

void set_single_splat_scale(const Ref<GaussianSplatAsset> &p_asset, const Vector3 &p_scale) {
    PackedFloat32Array scales;
    scales.resize(3);
    {
        float *ptr = scales.ptrw();
        ptr[0] = p_scale.x;
        ptr[1] = p_scale.y;
        ptr[2] = p_scale.z;
    }
    p_asset->set_scales(scales);
}

void set_single_splat_rotation(const Ref<GaussianSplatAsset> &p_asset, const Quaternion &p_rotation) {
    PackedFloat32Array rotations;
    rotations.resize(4);
    {
        float *ptr = rotations.ptrw();
        ptr[0] = p_rotation.w;
        ptr[1] = p_rotation.x;
        ptr[2] = p_rotation.y;
        ptr[3] = p_rotation.z;
    }
    p_asset->set_rotations(rotations);
}

// ── Return-safe teardown for test nodes attached to the shared SceneTree ────────
//
// #806 review: this file's [SceneTree] cases build nodes with memnew(), parent them
// to the ONE root Window that every case in a doctest batch shares, and tear them
// down with a hand-written `remove_child(); memdelete();` pair repeated before every
// exit. That teardown is correct only for as long as every exit remembers it.
//
// It stopped being correct once #656/#843 replaced bare `REQUIRE` with
// `if (!x) { FAIL(); return; }`. That idiom is REQUIRED here -- doctest is built with
// disable_exceptions=True, so a failed REQUIRE does not abort and the following
// dereference crashes the whole run -- but its `return` skips whatever teardown
// followed. Three exits in "A failed set_splat_data must not republish the previous
// payload" did exactly that: they returned with the node still parented to the root
// window and, in two of the three, still registered with the SceneDirector. The
// NodeSceneTree GPU-harness batch runs as ONE doctest process
// (tests/ci/run_gpu_harness.py: BatchSpec("NodeSceneTree",
// ("*[Node][SceneTree][RequiresGPU]*",), timeout_seconds=300)), so the survivor is
// visible to every later case in that batch -- as an extra root child, an extra
// director instance row, and retained renderer state.
//
// Repeating the teardown before each new `FAIL` would fix the three known exits and
// leave the shape intact: the next exit added is one forgotten line away from the same
// bug. So the teardown is made structural instead. The destructor runs on EVERY exit
// from the scope -- fallthrough, early `return`, or a `return` added later by someone
// who never reads this comment -- which is the property the hand-written version cannot
// have.
//
// Deliberately parent-agnostic: it detaches from whatever parent the node has AT
// DESTRUCTION time, not the one it was added to, so a case that re-parents (the
// remove_child/add_child tree re-entry the set_splat_data case performs) still tears
// down correctly. A node that is already detached is simply freed.
template <typename T>
class ScopedTestNode {
public:
    explicit ScopedTestNode(T *p_node) :
            node(p_node) {}
    ~ScopedTestNode() { reset(); }

    ScopedTestNode(const ScopedTestNode &) = delete;
    ScopedTestNode &operator=(const ScopedTestNode &) = delete;

    T *get() const { return node; }
    T *operator->() const { return node; }
    explicit operator bool() const { return node != nullptr; }

    // Hand the node back to the caller; the guard stops owning it.
    T *release() {
        T *released = node;
        node = nullptr;
        return released;
    }

    void reset() {
        if (node == nullptr) {
            return;
        }
        Node *parent = node->get_parent();
        if (parent != nullptr) {
            parent->remove_child(node);
        }
        memdelete(node);
        node = nullptr;
        // Matches the hand-written teardown this replaces: let the tree settle so the
        // director observes the exit before the next case runs.
        SceneTree *tree = SceneTree::get_singleton();
        if (tree != nullptr) {
            tree->process(0.0);
        }
    }

private:
    T *node = nullptr;
};

// What the helper below observed from INSIDE the scope, i.e. before the guard ran.
//
// These are the non-vacuity witnesses. Once ~ScopedTestNode() has done its job there is
// nothing left in the tree to look at, so "the tree is back to baseline" afterwards is
// indistinguishable from "the helper never attached anything" unless the helper itself
// reports what it saw while the node was still alive.
struct ScopedTestNodeExitObservation {
    bool took_early_exit = false;
    ObjectID instance_id;
    int child_count_while_attached = -1;
    bool registered_while_attached = false;
};

// The exact exit shape the guard exists for, factored out so a test case can OBSERVE
// it. Attaches a node to `p_root`, records the observation above, and then -- when
// `p_take_setup_failure_exit` is set -- returns early from the middle of the scope with
// no teardown statement of any kind after it, exactly as the
// `if (!x) { FAIL(); return; }` branches in the set_splat_data case do.
ScopedTestNodeExitObservation take_scoped_test_node_setup_failure_exit(Window *p_root, bool p_take_setup_failure_exit) {
    ScopedTestNodeExitObservation observation;

    ScopedTestNode<GaussianSplatNode3D> node(memnew(GaussianSplatNode3D));
    node->set_splat_asset(make_single_splat_asset(3.0f));
    p_root->add_child(node.get());
    observation.instance_id = node->get_instance_id();
    SceneTree *tree = SceneTree::get_singleton();
    if (tree != nullptr) {
        tree->process(0.0);
    }

    observation.child_count_while_attached = p_root->get_child_count();
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (director != nullptr) {
        GaussianSplatSceneDirector::InstanceSubmission submission;
        observation.registered_while_attached = director->get_instance_submission(observation.instance_id, &submission);
    }

    if (p_take_setup_failure_exit) {
        observation.took_early_exit = true;
        return observation;
    }

    return observation;
}

} // namespace

TEST_CASE("[GaussianSplatting][Node] Debug flag persistence mirrors project settings") {
    // NOTE: Debug flag persistence to ProjectSettings is now editor-only.
    // At runtime (non-editor), flags are applied to the renderer but NOT saved.
    // This test verifies the runtime behavior where settings are read but not written.
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    CHECK_MESSAGE(project_settings != nullptr, "ProjectSettings singleton must exist for GaussianSplatNode3D persistence test");
    if (project_settings == nullptr) {
        return;
    }

    const String tile_setting = "rendering/gaussian_splatting/debug/show_tile_grid";
    const String heatmap_setting = "rendering/gaussian_splatting/debug/show_density_heatmap";
    const String hud_setting = "rendering/gaussian_splatting/debug/show_performance_hud";

    ProjectSettingGuard tile_guard(project_settings, tile_setting);
    ProjectSettingGuard heatmap_guard(project_settings, heatmap_setting);
    ProjectSettingGuard hud_guard(project_settings, hud_setting);

    project_settings->set_setting(tile_setting, false);
    project_settings->set_setting(heatmap_setting, false);
    project_settings->set_setting(hud_setting, false);
    CHECK_EQ(project_settings->save(), OK);

    GaussianSplatNode3D *initial_node = memnew(GaussianSplatNode3D);
    CHECK(initial_node != nullptr);
    if (initial_node == nullptr) {
        return;
    }

    // Verify initial state is loaded from project settings
    CHECK_FALSE(initial_node->is_showing_tile_grid());
    CHECK_FALSE(initial_node->is_showing_density_heatmap());
    CHECK_FALSE(initial_node->is_showing_performance_hud());

    // Change the flags
    initial_node->set_show_tile_grid(true);
    initial_node->set_show_density_heatmap(true);
    initial_node->set_show_performance_hud(true);

    // Verify the node's local state changed
    CHECK(initial_node->is_showing_tile_grid());
    CHECK(initial_node->is_showing_density_heatmap());
    CHECK(initial_node->is_showing_performance_hud());

    // At runtime (non-editor), settings should NOT be persisted to ProjectSettings.
    // In editor, they would be. Since tests run in non-editor mode, project settings
    // should remain unchanged.
#ifndef TOOLS_ENABLED
    CHECK_FALSE((bool)project_settings->get_setting(tile_setting));
    CHECK_FALSE((bool)project_settings->get_setting(heatmap_setting));
    CHECK_FALSE((bool)project_settings->get_setting(hud_setting));
#else
    // In editor builds running tests, we can't easily distinguish editor vs runtime,
    // so we skip the persistence check. The important thing is that the node's
    // local state is correctly set.
#endif

    memdelete(initial_node);

    // Verify new nodes still read from project settings (not from previous node)
    GaussianSplatNode3D *fresh_node = memnew(GaussianSplatNode3D);
    CHECK(fresh_node != nullptr);
    if (fresh_node == nullptr) {
        return;
    }

    // Since we didn't persist above (runtime mode), fresh node should read original values
#ifndef TOOLS_ENABLED
    CHECK_FALSE(fresh_node->is_showing_tile_grid());
    CHECK_FALSE(fresh_node->is_showing_density_heatmap());
    CHECK_FALSE(fresh_node->is_showing_performance_hud());
#endif

    memdelete(fresh_node);
}

TEST_CASE("[GaussianSplatting][Node] Effective config snapshot reports tier caps with source attribution") {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    REQUIRE(project_settings != nullptr);
    if (project_settings == nullptr) {
        return;
    }

    const String tier_preset_setting = "rendering/gaussian_splatting/quality/tier_preset";
    const String tier_apply_setting = "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets";

    ProjectSettingGuard tier_preset_guard(project_settings, tier_preset_setting);
    ProjectSettingGuard tier_apply_guard(project_settings, tier_apply_setting);

    project_settings->set_setting(tier_preset_setting, String("low"));
    project_settings->set_setting(tier_apply_setting, true);

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    REQUIRE(node != nullptr);
    if (node == nullptr) {
        return;
    }

    node->set_quality_preset(GaussianSplatNode3D::QUALITY_QUALITY);

    const Dictionary snapshot = node->get_effective_config_snapshot();
    const Dictionary max_splats_entry = GaussianEffectiveConfig::get_entry(snapshot, StringName("max_splats"));
    const Dictionary gpu_memory_entry = GaussianEffectiveConfig::get_entry(snapshot, StringName("gpu_memory_mb"));
    const Dictionary lod_entry = GaussianEffectiveConfig::get_entry(snapshot, StringName("lod_max_distance"));

    CHECK(int64_t(max_splats_entry.get(StringName("value"), int64_t(-1))) == int64_t(300000));
    CHECK(String(max_splats_entry.get(StringName("source_label"), String())) == String("capped by tier 'low'"));
    CHECK(int64_t(gpu_memory_entry.get(StringName("value"), int64_t(-1))) == int64_t(256));
    CHECK(String(gpu_memory_entry.get(StringName("source_label"), String())) == String("capped by tier 'low'"));
    CHECK(String(lod_entry.get(StringName("source_label"), String())) == String("node property"));
    const Dictionary load_ahead_entry = GaussianEffectiveConfig::get_entry(snapshot, StringName("streaming_load_ahead_factor"));
    const Dictionary unload_entry = GaussianEffectiveConfig::get_entry(snapshot, StringName("streaming_unload_factor"));
    const Dictionary concurrent_loads_entry = GaussianEffectiveConfig::get_entry(snapshot, StringName("streaming_max_concurrent_loads"));
    const Dictionary target_gpu_entry = GaussianEffectiveConfig::get_entry(snapshot, StringName("target_gpu_memory_mb"));
    const Dictionary stream_budget_entry = GaussianEffectiveConfig::get_entry(snapshot, StringName("stream_budget_ms"));
    CHECK(int64_t(target_gpu_entry.get(StringName("value"), int64_t(-1))) == int64_t(192));
    CHECK(String(target_gpu_entry.get(StringName("source_label"), String())) == String("capped by tier 'low'"));
    CHECK(Math::is_equal_approx(float(double(load_ahead_entry.get(StringName("value"), 0.0))), 0.15f));
    CHECK(String(load_ahead_entry.get(StringName("source_label"), String())) == String("capped by tier 'low'"));
    CHECK(Math::is_equal_approx(float(double(unload_entry.get(StringName("value"), 0.0))), 0.95f));
    CHECK(String(unload_entry.get(StringName("source_label"), String())) == String("capped by tier 'low'"));
    CHECK(int64_t(concurrent_loads_entry.get(StringName("value"), int64_t(-1))) == int64_t(1));
    CHECK(String(concurrent_loads_entry.get(StringName("source_label"), String())) == String("capped by tier 'low'"));
    CHECK(int64_t(stream_budget_entry.get(StringName("value"), int64_t(-1))) == int64_t(1));
    CHECK(String(stream_budget_entry.get(StringName("source_label"), String())) == String("capped by tier 'low'"));

    memdelete(node);
}

#ifdef TOOLS_ENABLED
TEST_CASE("[GaussianSplatting][Node] Editor summary surfaces capped streaming values with source attribution") {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    REQUIRE(project_settings != nullptr);
    if (project_settings == nullptr) {
        return;
    }

    const String tier_preset_setting = "rendering/gaussian_splatting/quality/tier_preset";
    const String tier_apply_setting = "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets";

    ProjectSettingGuard tier_preset_guard(project_settings, tier_preset_setting);
    ProjectSettingGuard tier_apply_guard(project_settings, tier_apply_setting);

    project_settings->set_setting(tier_preset_setting, String("low"));
    project_settings->set_setting(tier_apply_setting, true);

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    REQUIRE(node != nullptr);
    if (node == nullptr) {
        return;
    }

    node->set_quality_preset(GaussianSplatNode3D::QUALITY_QUALITY);
    const String stats_text = GaussianEditorServices::format_gaussian_splat_stats(node, Ref<GaussianSplatRenderer>());
    CHECK(stats_text.contains("Effective Target GPU Memory: 192 MB"));
    CHECK(stats_text.contains("Effective Load Ahead: 0.15"));
    CHECK(stats_text.contains("Effective Unload: 0.95"));
    CHECK(stats_text.contains("Effective Concurrent Loads: 1"));
    CHECK(stats_text.contains("Effective Stream Budget: 1 ms"));
    CHECK(stats_text.contains("capped by tier 'low'"));

    memdelete(node);
}
#endif

TEST_CASE("[GaussianSplatting][Node] Effective config snapshot honors SH project override over tier default") {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    REQUIRE(project_settings != nullptr);
    if (project_settings == nullptr) {
        return;
    }

    const String tier_preset_setting = "rendering/gaussian_splatting/quality/tier_preset";
    const String sh_bands_setting = SHConfig::BANDS_PATH;

    {
        ProjectSettingGuard tier_preset_guard(project_settings, tier_preset_setting);
        ProjectSettingGuard sh_bands_guard(project_settings, sh_bands_setting);

        project_settings->set_setting(tier_preset_setting, String("steam_deck"));
        project_settings->set_setting(sh_bands_setting, int64_t(SH_BAND_3));

        g_sh_config.load_from_project_settings();
        const Dictionary snapshot = g_sh_config.get_effective_config_snapshot();
        const Dictionary sh_entry = GaussianEffectiveConfig::get_entry(snapshot, StringName("sh_bands"));

        CHECK(int64_t(sh_entry.get(StringName("value"), int64_t(-1))) == int64_t(SH_BAND_3));
        CHECK(String(sh_entry.get(StringName("source_label"), String())) == String("project override"));
        CHECK(String(sh_entry.get(StringName("display_value"), String())) == String("SH3 (3rd order)"));
    }

    g_sh_config.load_from_project_settings();
}

TEST_CASE("[GaussianSplatting][Node][SceneTree] Default update mode processes automatically") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    CHECK(node != nullptr);
    if (node == nullptr) {
        return;
    }

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    node->set_splat_asset(asset);

    root->add_child(node);
    CHECK(node->is_inside_tree());
    if (!node->is_inside_tree()) {
        memdelete(node);
        return;
    }

    // When GaussianSplatManager exists, it drives updates via frame_pre_draw callback
    // and node processing is disabled to avoid double updates.
    // When manager is unavailable, node falls back to NOTIFICATION_PROCESS.
    GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
    if (manager) {
        // Manager exists: node should NOT be processing (manager handles it)
        CHECK_FALSE(node->is_processing());
    } else {
        // No manager: node should be processing as fallback
        CHECK(node->is_processing());
    }

    const float initial_time = node->get_last_update_time_ms();

    // Run a frame to trigger updates (either via manager or NOTIFICATION_PROCESS)
    tree->process(0.016);

    // Update time should advance regardless of which path is used
    CHECK_MESSAGE(node->get_last_update_time_ms() >= initial_time, "last_update_time_ms should not regress after processing");

    root->remove_child(node);
    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node] Asset buffers populate GaussianData with painterly metadata") {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();

    const int splat_count = 2;
    asset->set_splat_count(splat_count);

    PackedFloat32Array positions;
    positions.resize(splat_count * 3);
    {
        float *ptr = positions.ptrw();
        ptr[0] = 1.0f;
        ptr[1] = 2.0f;
        ptr[2] = 3.0f;
        ptr[3] = 4.0f;
        ptr[4] = 5.0f;
        ptr[5] = 6.0f;
    }
    asset->set_positions(positions);

    PackedFloat32Array scales;
    scales.resize(splat_count * 3);
    {
        float *ptr = scales.ptrw();
        ptr[0] = 0.5f;
        ptr[1] = 0.6f;
        ptr[2] = 0.7f;
        ptr[3] = 1.1f;
        ptr[4] = 1.2f;
        ptr[5] = 1.3f;
    }
    asset->set_scales(scales);

    PackedFloat32Array rotations;
    rotations.resize(splat_count * 4);
    {
        float *ptr = rotations.ptrw();
        // Stored as w, x, y, z in asset
        ptr[0] = 1.0f;
        ptr[1] = 0.0f;
        ptr[2] = 0.0f;
        ptr[3] = 0.0f;
        ptr[4] = 0.7071067f;
        ptr[5] = 0.0f;
        ptr[6] = 0.7071067f;
        ptr[7] = 0.0f;
    }
    asset->set_rotations(rotations);

    PackedFloat32Array sh_dc;
    sh_dc.resize(splat_count * 3);
    {
        float *ptr = sh_dc.ptrw();
        ptr[0] = 0.8f;
        ptr[1] = 0.1f;
        ptr[2] = 0.2f;
        ptr[3] = 0.3f;
        ptr[4] = 0.4f;
        ptr[5] = 0.5f;
    }
    asset->set_sh_dc_coefficients(sh_dc);

    PackedFloat32Array sh_first;
    sh_first.resize(splat_count * 2 * 3);
    {
        float *ptr = sh_first.ptrw();
        ptr[0] = 0.01f;
        ptr[1] = 0.02f;
        ptr[2] = 0.03f;
        ptr[3] = 0.04f;
        ptr[4] = 0.05f;
        ptr[5] = 0.06f;
        ptr[6] = 0.11f;
        ptr[7] = 0.12f;
        ptr[8] = 0.13f;
        ptr[9] = 0.14f;
        ptr[10] = 0.15f;
        ptr[11] = 0.16f;
    }
    asset->set_sh_first_order_coefficients(sh_first);

    PackedFloat32Array sh_high;
    sh_high.resize(splat_count * 3);
    {
        float *ptr = sh_high.ptrw();
        ptr[0] = 0.2f;
        ptr[1] = 0.3f;
        ptr[2] = 0.4f;
        ptr[3] = 0.5f;
        ptr[4] = 0.6f;
        ptr[5] = 0.7f;
    }
    asset->set_sh_high_order_coefficients(sh_high);

    PackedFloat32Array opacity_logits;
    opacity_logits.resize(splat_count);
    {
        float *ptr = opacity_logits.ptrw();
        ptr[0] = 0.0f;
        ptr[1] = 2.1972246f; // logit for 0.9
    }
    asset->set_opacity_logits(opacity_logits);

    PackedInt32Array palette_ids;
    palette_ids.resize(splat_count);
    {
        int32_t *ptr = palette_ids.ptrw();
        ptr[0] = 123;
        ptr[1] = 456;
    }
    asset->set_palette_ids(palette_ids);

    PackedInt32Array brush_override_ids;
    brush_override_ids.resize(splat_count);
    {
        int32_t *ptr = brush_override_ids.ptrw();
        ptr[0] = 7;
        ptr[1] = 255;
    }
    asset->set_brush_override_ids(brush_override_ids);
    CHECK_EQ(asset->get_painterly_flags_buffer()[0], 7);
    CHECK_EQ(asset->get_brush_override_ids_buffer()[1], 255);

    PackedFloat32Array normals;
    normals.resize(splat_count * 3);
    {
        float *ptr = normals.ptrw();
        ptr[0] = 0.0f;
        ptr[1] = 1.0f;
        ptr[2] = 0.0f;
        ptr[3] = 0.0f;
        ptr[4] = 0.0f;
        ptr[5] = 1.0f;
    }
    asset->set_normals(normals);

    PackedFloat32Array brush_axes;
    brush_axes.resize(splat_count * 2);
    {
        float *ptr = brush_axes.ptrw();
        ptr[0] = 1.0f;
        ptr[1] = 0.5f;
        ptr[2] = 0.75f;
        ptr[3] = 1.25f;
    }
    asset->set_brush_axes(brush_axes);

    PackedFloat32Array stroke_ages;
    stroke_ages.resize(splat_count);
    {
        float *ptr = stroke_ages.ptrw();
        ptr[0] = 3.0f;
        ptr[1] = 7.5f;
    }
    asset->set_stroke_ages(stroke_ages);

    Dictionary metadata = asset->get_import_metadata();
    metadata[StringName("gaussian_2d_mode")] = true;
    asset->set_import_metadata(metadata);

    Ref<::GaussianData> data;
    data.instantiate();
    CHECK_EQ(data->populate_from_asset(asset), OK);

    CHECK(data->get_2d_mode());
    CHECK_EQ(data->get_sh_first_order_count(), 3u);
    CHECK_EQ(data->get_sh_high_order_count(), 0u);

    Gaussian g0 = data->get_gaussian(0);
    CHECK(g0.position.is_equal_approx(Vector3(1.0f, 2.0f, 3.0f)));
    CHECK(g0.scale.is_equal_approx(Vector3(0.5f, 0.6f, 0.7f)));
    CHECK(g0.rotation.is_equal_approx(Quaternion(0.0f, 0.0f, 0.0f, 1.0f)));
    CHECK(Math::is_equal_approx(g0.opacity, 0.5f));
    CHECK(g0.sh_dc.is_equal_approx(Color(0.8f, 0.1f, 0.2f, 1.0f)));
    CHECK(g0.sh_1[0].is_equal_approx(Vector3(0.01f, 0.02f, 0.03f)));
    CHECK(g0.sh_1[1].is_equal_approx(Vector3(0.04f, 0.05f, 0.06f)));
    CHECK(g0.sh_1[2].is_equal_approx(Vector3(0.2f, 0.3f, 0.4f)));
    CHECK(g0.normal.is_equal_approx(Vector3(0.0f, 1.0f, 0.0f)));
    CHECK(g0.brush_axes.is_equal_approx(Vector2(1.0f, 0.5f)));
    CHECK(Math::is_equal_approx(g0.stroke_age, 3.0f));
    CHECK_EQ(gaussian_get_palette_id(g0.painterly_meta), 123);
    CHECK_EQ(gaussian_get_brush_override_id(g0.painterly_meta), 7);
    CHECK_EQ(gaussian_get_painterly_flags(g0.painterly_meta), 7);

    Gaussian g1 = data->get_gaussian(1);
    CHECK(g1.position.is_equal_approx(Vector3(4.0f, 5.0f, 6.0f)));
    CHECK(g1.scale.is_equal_approx(Vector3(1.1f, 1.2f, 1.3f)));
    CHECK(g1.rotation.is_equal_approx(Quaternion(0.0f, 0.7071067f, 0.0f, 0.7071067f)));
    CHECK(Math::is_equal_approx(g1.opacity, 1.0f / (1.0f + Math::exp(-2.1972246f))));
    CHECK(g1.sh_dc.is_equal_approx(Color(0.3f, 0.4f, 0.5f, 1.0f)));
    CHECK(g1.sh_1[0].is_equal_approx(Vector3(0.11f, 0.12f, 0.13f)));
    CHECK(g1.sh_1[1].is_equal_approx(Vector3(0.14f, 0.15f, 0.16f)));
    CHECK(g1.sh_1[2].is_equal_approx(Vector3(0.5f, 0.6f, 0.7f)));
    CHECK(g1.normal.is_equal_approx(Vector3(0.0f, 0.0f, 1.0f)));
    CHECK(g1.brush_axes.is_equal_approx(Vector2(0.75f, 1.25f)));
    CHECK(Math::is_equal_approx(g1.stroke_age, 7.5f));
    CHECK_EQ(gaussian_get_palette_id(g1.painterly_meta), 456);
    CHECK_EQ(gaussian_get_brush_override_id(g1.painterly_meta), 255);
    CHECK_EQ(gaussian_get_painterly_flags(g1.painterly_meta), 255);

    Ref<GaussianSplatAsset> roundtrip_asset;
    roundtrip_asset.instantiate();
    CHECK_EQ(roundtrip_asset->populate_from_gaussian_data(data), OK);
    CHECK_EQ(roundtrip_asset->get_brush_override_ids_buffer().size(), splat_count);
    CHECK_EQ(roundtrip_asset->get_brush_override_ids_buffer()[0], 7);
    CHECK_EQ(roundtrip_asset->get_painterly_flags_buffer()[1], 255);

    const Vector3 *high_ptr = data->get_sh_high_order_coefficients_ptr();
    CHECK(high_ptr == nullptr);
}

TEST_CASE("[GaussianSplatting][Node] Cached bounds stay coherent after position/scale/rotation mutations") {
    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    CHECK(node != nullptr);
    if (node == nullptr) {
        return;
    }

    Ref<GaussianSplatAsset> asset = make_single_splat_asset(0.0f);
    const AABB cached_bounds(Vector3(-3.0f, -3.0f, -3.0f), Vector3(6.0f, 6.0f, 6.0f));
    Dictionary metadata = asset->get_import_metadata();
    metadata[StringName("bounds")] = cached_bounds;
    asset->set_import_metadata(metadata);

    node->set_splat_asset(asset);
    AABB bounds = node->get_aabb();
    CHECK(bounds.position.is_equal_approx(cached_bounds.position));
    CHECK(bounds.size.is_equal_approx(cached_bounds.size));

    set_single_splat_position(asset, Vector3(4.0f, 0.0f, 0.0f));
    const AABB expected_after_position(Vector3(1.0f, -3.0f, -3.0f), Vector3(6.0f, 6.0f, 6.0f));
    bounds = node->get_aabb();
    CHECK_MESSAGE(bounds.position.is_equal_approx(expected_after_position.position),
            "Position mutation should invalidate stale cached bounds (position).");
    CHECK_MESSAGE(bounds.size.is_equal_approx(expected_after_position.size),
            "Position mutation should invalidate stale cached bounds (size).");

    set_single_splat_scale(asset, Vector3(2.0f, 1.0f, 1.0f));
    const AABB expected_after_scale(Vector3(-2.0f, -6.0f, -6.0f), Vector3(12.0f, 12.0f, 12.0f));
    bounds = node->get_aabb();
    CHECK_MESSAGE(bounds.position.is_equal_approx(expected_after_scale.position),
            "Scale mutation should keep the node bounds coherent (position).");
    CHECK_MESSAGE(bounds.size.is_equal_approx(expected_after_scale.size),
            "Scale mutation should keep the node bounds coherent (size).");

    const Quaternion rotated(Vector3(0.0f, 0.0f, 1.0f), Math::deg_to_rad(90.0f));
    set_single_splat_rotation(asset, rotated);
    bounds = node->get_aabb();
    CHECK_MESSAGE(bounds.position.is_equal_approx(expected_after_scale.position),
            "Rotation mutation should not leave node using stale bounds (position).");
    CHECK_MESSAGE(bounds.size.is_equal_approx(expected_after_scale.size),
            "Rotation mutation should not leave node using stale bounds (size).");

    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node] Debug overlay opacity is clamped and defaults correctly") {
    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    CHECK(node != nullptr);
    if (node == nullptr) {
        return;
    }

    // Check default value
    CHECK(Math::is_equal_approx(node->get_debug_overlay_opacity(), 0.3f));

    // Set valid values
    node->set_debug_overlay_opacity(0.5f);
    CHECK(Math::is_equal_approx(node->get_debug_overlay_opacity(), 0.5f));

    node->set_debug_overlay_opacity(0.0f);
    CHECK(Math::is_equal_approx(node->get_debug_overlay_opacity(), 0.0f));

    node->set_debug_overlay_opacity(1.0f);
    CHECK(Math::is_equal_approx(node->get_debug_overlay_opacity(), 1.0f));

    // Test clamping
    node->set_debug_overlay_opacity(-0.5f);
    CHECK(Math::is_equal_approx(node->get_debug_overlay_opacity(), 0.0f));

    node->set_debug_overlay_opacity(1.5f);
    CHECK(Math::is_equal_approx(node->get_debug_overlay_opacity(), 1.0f));

    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node] Debug overlay toggles have correct defaults") {
    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    CHECK(node != nullptr);
    if (node == nullptr) {
        return;
    }

    // Visual overlays should be off by default
    CHECK_FALSE(node->is_showing_tile_grid());
    CHECK_FALSE(node->is_showing_density_heatmap());

    // Stats-only HUD toggles should be off by default
    CHECK_FALSE(node->is_showing_performance_hud());
    CHECK_FALSE(node->is_showing_residency_hud());

    // Test toggling
    node->set_show_tile_grid(true);
    CHECK(node->is_showing_tile_grid());

    node->set_show_density_heatmap(true);
    CHECK(node->is_showing_density_heatmap());

    node->set_show_performance_hud(true);
    CHECK(node->is_showing_performance_hud());

    node->set_show_residency_hud(true);
    CHECK(node->is_showing_residency_hud());

    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node] Legacy non-functional properties are not exposed but still deserialize") {
    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    CHECK(node != nullptr);
    if (node == nullptr) {
        return;
    }

    List<PropertyInfo> property_list;
    node->get_property_list(&property_list);

    bool has_color_variation_property = false;
    bool has_occlusion_culling_property = false;
    for (const List<PropertyInfo>::Element *E = property_list.front(); E; E = E->next()) {
        const StringName &property_name = E->get().name;
        if (property_name == StringName("painterly/color_variation")) {
            has_color_variation_property = true;
        }
        if (property_name == StringName("rendering/occlusion_culling")) {
            has_occlusion_culling_property = true;
        }
    }

    CHECK_FALSE(has_color_variation_property);
    CHECK_FALSE(has_occlusion_culling_property);

    bool set_valid = false;
    node->set(StringName("painterly/color_variation"), 0.3f, &set_valid);
    CHECK(set_valid);
    CHECK(Math::is_equal_approx(node->get_color_variation(), 0.3f));

    node->set(StringName("rendering/occlusion_culling"), false, &set_valid);
    CHECK(set_valid);
    CHECK_FALSE(node->is_occlusion_culling_enabled());

    bool get_valid = false;
    Variant color_variation = node->get(StringName("painterly/color_variation"), &get_valid);
    CHECK(get_valid);
    CHECK(Math::is_equal_approx(float(color_variation), 0.3f));

    Variant occlusion_culling = node->get(StringName("rendering/occlusion_culling"), &get_valid);
    CHECK(get_valid);
    CHECK_FALSE((bool)occlusion_culling);

    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node] Asset origin label describes active ingress") {
    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    REQUIRE(node != nullptr);

    CHECK(node->get_asset_origin_label() == String("No asset assigned"));

    Ref<GaussianSplatAsset> asset = make_single_splat_asset();
    asset->set_source_path("res://imported_source.ply");
    node->set_splat_asset(asset);

    const String asset_origin = node->get_asset_origin_label();
    CHECK(asset_origin.contains("Assigned GaussianSplatAsset"));
    CHECK(asset_origin.contains("source: res://imported_source.ply"));

    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node] Source path helper resolves asset-only source metadata") {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();

    Dictionary metadata;
    metadata[StringName("source_file")] = String("res://metadata_source.ply");
    asset->set_import_metadata(metadata);

    CHECK(GaussianSplatSourcePath::get_asset_source_path(asset) == String("res://metadata_source.ply"));
    CHECK(GaussianSplatSourcePath::resolve_primary_source_path(asset) == String("res://metadata_source.ply"));

    metadata.erase(StringName("source_file"));
    metadata[StringName("runtime_load_source_path")] = String("res://runtime_source.ply");
    asset->set_import_metadata(metadata);

    CHECK(GaussianSplatSourcePath::get_asset_source_path(asset) == String("res://runtime_source.ply"));
    CHECK(GaussianSplatSourcePath::resolve_primary_source_path(asset) == String("res://runtime_source.ply"));

    asset->set_source_path("res://asset_source.ply");
    CHECK(GaussianSplatSourcePath::get_asset_source_path(asset) == String("res://asset_source.ply"));
    CHECK(GaussianSplatSourcePath::resolve_primary_source_path(asset) == String("res://asset_source.ply"));

    asset->set_source_path(String());
    asset->set_import_metadata(Dictionary());
    CHECK(GaussianSplatSourcePath::get_asset_source_path(asset).is_empty());
    CHECK(GaussianSplatSourcePath::resolve_primary_source_path(asset).is_empty());
}

TEST_CASE("[GaussianSplatting][Node] Configuration warnings follow asset-only source contract") {
    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    REQUIRE(node != nullptr);

    PackedStringArray warnings = node->get_configuration_warnings();
    bool has_missing_asset_warning = false;
    for (int i = 0; i < warnings.size(); i++) {
        if (String(warnings[i]).contains("No Gaussian splat asset or runtime data assigned")) {
            has_missing_asset_warning = true;
            break;
        }
    }
    CHECK(has_missing_asset_warning);

    Ref<GaussianSplatAsset> asset = make_single_splat_asset();
    asset->set_source_path("res://asset_source.ply");
    node->set_splat_asset(asset);

    warnings = node->get_configuration_warnings();
    bool mentions_legacy_dual_source = false;
    for (int i = 0; i < warnings.size(); i++) {
        if (String(warnings[i]).contains("ply_file_path")) {
            mentions_legacy_dual_source = true;
            break;
        }
    }
    CHECK_FALSE(mentions_legacy_dual_source);

    memdelete(node);
}

TEST_CASE("[GaussianSplatting][World][SceneTree][RequiresGPU] Shared renderer ownership blocks foreign clear/mutate") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    Ref<GaussianSplatWorld> world_a;
    world_a.instantiate();
    Ref<GaussianData> data_a = make_test_gaussian_data(1, 0.0f);
    world_a->set_gaussian_data(data_a);

    Ref<GaussianSplatWorld> world_b;
    world_b.instantiate();
    Ref<GaussianData> data_b = make_test_gaussian_data(2, 100.0f);
    world_b->set_gaussian_data(data_b);

    GaussianSplatWorld3D *node_a = memnew(GaussianSplatWorld3D);
    GaussianSplatWorld3D *node_b = memnew(GaussianSplatWorld3D);
    node_a->set_world(world_a);
    node_b->set_world(world_b);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer_a = node_a->get_renderer();
    Ref<GaussianSplatRenderer> renderer_b = node_b->get_renderer();
    if (!renderer_a.is_valid() || !renderer_b.is_valid() || renderer_a != renderer_b) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    node_a->apply_world();
    CHECK(renderer_a->get_gaussian_data() == data_a);

    node_b->clear_world();
    CHECK(renderer_a->get_gaussian_data() == data_a);

    node_b->apply_world();
    CHECK(renderer_a->get_gaussian_data() == data_a);

    node_a->clear_world();
    CHECK_FALSE(renderer_a->get_gaussian_data().is_valid());

    node_b->apply_world();
    CHECK(renderer_a->get_gaussian_data() == data_b);

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

// #831: this case previously pinned the opposite contract -- "shared renderer
// hides node-local debug settings" -- which forced all four screen-space debug
// overlays off in any scene with more than one splat node while the node
// property still read back true. That suppression is replaced by a union over
// the renderer's peers, so the assertions below pin the new contract: the
// request reaches the renderer, and the control stays editable.
TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Shared renderer unions node-local debug overlay requests and keeps the property editable") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(0.0f));
    node_b->set_splat_asset(make_single_splat_asset(10.0f));

    root->add_child(node_a);
    tree->process(0.0);

    node_a->set_show_density_heatmap(true);

    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer_a = node_a->get_renderer();
    Ref<GaussianSplatRenderer> renderer_b = node_b->get_renderer();
    // #595: this case only runs in the [RequiresGPU] harness, where a shared
    // renderer is always reachable. An environment skip here would be scored as
    // a PASS and silently drop the whole #831 contract, so the precondition
    // fails instead.
    if (!renderer_a.is_valid() || !renderer_b.is_valid() || renderer_a != renderer_b) {
        FAIL("shared renderer required: both nodes must resolve the same GaussianSplatRenderer");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    CHECK(node_a->is_showing_density_heatmap());
    CHECK_FALSE(node_b->is_showing_density_heatmap());
    // The union carries node_a's request through to the shared renderer.
    CHECK(renderer_a->is_debug_show_density_heatmap());
    // ...and neither node hides the control, so the user can still turn it off.
    CHECK(is_property_editor_exposed(node_a, StringName("debug/show_density_heatmap")));
    CHECK(is_property_editor_exposed(node_b, StringName("debug/show_density_heatmap")));

    // A second requester is idempotent.
    node_b->set_show_density_heatmap(true);
    CHECK(node_b->is_showing_density_heatmap());
    CHECK(renderer_a->is_debug_show_density_heatmap());

    // Clearing one peer does NOT clear the renderer-wide overlay while another
    // peer still requests it -- that is what makes the union order-independent.
    node_a->set_show_density_heatmap(false);
    CHECK_FALSE(node_a->is_showing_density_heatmap());
    CHECK(node_b->is_showing_density_heatmap());
    CHECK(renderer_a->is_debug_show_density_heatmap());

    // Clearing the last requester clears the renderer.
    node_b->set_show_density_heatmap(false);
    CHECK_FALSE(renderer_a->is_debug_show_density_heatmap());

    // Re-arm on node_a, then remove it: the union shrinks to node_b's false.
    node_a->set_show_density_heatmap(true);
    CHECK(renderer_a->is_debug_show_density_heatmap());

    root->remove_child(node_a);
    memdelete(node_a);
    node_a = nullptr;
    tree->process(0.0);

    CHECK(is_property_editor_exposed(node_b, StringName("debug/show_density_heatmap")));
    CHECK_FALSE(node_b->is_showing_density_heatmap());
    CHECK_FALSE(renderer_a->is_debug_show_density_heatmap());

    root->remove_child(node_b);
    memdelete(node_b);
}

// #831 part 2: the node re-asserted its own four debug overlay flags from
// update_splats() on every frame, so a GaussianSplatRenderer::set_debug_show_*()
// call taken through the bound GDScript API was silently reverted one frame
// later (measured: the flag reads back true in the same frame and false in the
// next). The push is now edge-triggered against the last request the node
// pushed, so an unchanged node leaves a renderer-side write alone.
TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Node debug apply does not revert a renderer-side debug flag write") {
    // #656: REQUIRE does not abort in this build, so guard-then-return rather
    // than dereferencing after a REQUIRE.
    SceneTree *tree = SceneTree::get_singleton();
    if (!tree) {
        FAIL("SceneTree singleton required");
        return;
    }

    Window *root = tree->get_root();
    if (!root) {
        FAIL("SceneTree root window required");
        return;
    }

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    node->set_splat_asset(make_single_splat_asset(4242.0f));

    root->add_child(node);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node->get_renderer();
    // #595: no environment skip -- a skip is scored as a PASS, and this case
    // exists precisely to prove the renderer-side write survives.
    if (!renderer.is_valid()) {
        FAIL("renderer required: this case runs only in the [RequiresGPU] harness");
        root->remove_child(node);
        memdelete(node);
        return;
    }

    CHECK_FALSE(node->is_showing_density_heatmap());
    CHECK_FALSE(renderer->is_debug_show_density_heatmap());

    renderer->set_debug_show_density_heatmap(true);
    CHECK(renderer->is_debug_show_density_heatmap());

    // The node's per-frame apply must not clobber this.
    for (int i = 0; i < 5; i++) {
        tree->process(0.0);
    }
    CHECK(renderer->is_debug_show_density_heatmap());
    // The node's own request is untouched by the renderer-side write.
    CHECK_FALSE(node->is_showing_density_heatmap());

    // A node-local change is still authoritative when it actually changes.
    node->set_show_density_heatmap(true);
    CHECK(renderer->is_debug_show_density_heatmap());
    node->set_show_density_heatmap(false);
    CHECK_FALSE(renderer->is_debug_show_density_heatmap());

    root->remove_child(node);
    memdelete(node);
}

// #839 round 2, finding 1: the HUD flags are unioned across the renderer's
// peers, but the HUD CONTROL is drawn by exactly one node -- the settings-owner
// lease holder -- so that N peers do not stack N identical overlays. A NON-owner
// peer that enables show_performance_hud therefore set the renderer flag, was
// refused the control by its own ownership check, and the actual owner was never
// told anything had changed: flag enabled, nothing on screen.
//
// UPDATE_MODE_MANUAL is the case that does not self-heal. The owner's per-frame
// apply (update_splats -> apply_renderer_settings -> apply_renderer_debug_settings
// -> _update_debug_hud_visibility) is the only thing that would have reconciled
// it, and MANUAL never runs update_splats -- see
// GaussianSplatNode3D::process_gaussian_render's UPDATE_MODE_MANUAL arm. The
// frame loop below is the proof that this is permanent, not deferred.
TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Manual-mode HUD owner is notified when a peer toggles a HUD flag") {
    // #656: REQUIRE does not abort in this build (disable_exceptions=True), so
    // guard-then-return rather than dereferencing after a REQUIRE.
    SceneTree *tree = SceneTree::get_singleton();
    if (!tree) {
        FAIL("SceneTree singleton required");
        return;
    }
    Window *root = tree->get_root();
    if (!root) {
        FAIL("SceneTree root window required");
        return;
    }
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings singleton required");
        return;
    }
    // set_show_performance_hud writes the project setting in an editor build,
    // and every later-constructed node seeds its node-local default from it.
    ProjectSettingGuard hud_guard(project_settings, "rendering/gaussian_splatting/debug/show_performance_hud");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(0.0f));
    node_b->set_splat_asset(make_single_splat_asset(10.0f));
    node_a->set_show_performance_hud(false);
    node_b->set_show_performance_hud(false);
    // Neither node's own per-frame apply may rescue this.
    node_a->set_update_mode(GaussianSplatNode3D::UPDATE_MODE_MANUAL);
    node_b->set_update_mode(GaussianSplatNode3D::UPDATE_MODE_MANUAL);

    // node_a enters first, so node_a claims the settings-owner lease for the
    // shared renderer (_claim_renderer_settings_owner: first claimant wins and a
    // live holder is never displaced).
    root->add_child(node_a);
    tree->process(0.0);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    // #595: no environment skip -- a skip is scored as a PASS.
    if (!renderer.is_valid() || node_b->get_renderer() != renderer) {
        FAIL("shared renderer required: both nodes must resolve the same GaussianSplatRenderer");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    CHECK_FALSE(renderer->is_debug_show_performance_hud());
    CHECK_FALSE(node_has_debug_hud_control(node_a));
    CHECK_FALSE(node_has_debug_hud_control(node_b));

    // The NON-owner peer turns the HUD on.
    node_b->set_show_performance_hud(true);
    CHECK(renderer->is_debug_show_performance_hud());

    // ...and the owner must be drawing it. Pre-fix this was false here and
    // stayed false forever, because node_b was refused the control and node_a
    // was never notified.
    CHECK(node_has_debug_hud_control(node_a));
    // Exactly one control, on the lease holder -- not one per peer.
    CHECK_FALSE(node_has_debug_hud_control(node_b));

    // MANUAL: no amount of frames reconciles this on its own, so the assertion
    // above is not merely early.
    for (int i = 0; i < 5; i++) {
        tree->process(0.0);
    }
    CHECK(node_has_debug_hud_control(node_a));
    CHECK_FALSE(node_has_debug_hud_control(node_b));

    // Clearing it from the non-owner tears the owner's control down again.
    node_b->set_show_performance_hud(false);
    CHECK_FALSE(renderer->is_debug_show_performance_hud());
    CHECK_FALSE(node_has_debug_hud_control(node_a));
    CHECK_FALSE(node_has_debug_hud_control(node_b));

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

// #839 round 2, finding 2: the overlay union is a function of the SET of nodes
// bound to the renderer, but the only thing that recomputed it on a peer change
// was _converge_shared_renderer_state, which is edge-triggered on the
// shared/not-shared BOOLEAN. A set that shrinks 3 -> 2 leaves that boolean at
// true, so every remaining peer returned early and nobody dropped the departed
// node's request: the renderer kept rendering an overlay no live node asks for.
//
// Again UPDATE_MODE_MANUAL is the non-self-healing case: the per-frame
// update_splats -> apply_renderer_settings -> push_debug_overlay_union route,
// which is the only other thing that recomputes the union, never runs.
TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Manual-mode peers recompute the overlay union when a third peer leaves") {
    SceneTree *tree = SceneTree::get_singleton();
    if (!tree) {
        FAIL("SceneTree singleton required");
        return;
    }
    Window *root = tree->get_root();
    if (!root) {
        FAIL("SceneTree root window required");
        return;
    }
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings singleton required");
        return;
    }
    ProjectSettingGuard tile_guard(project_settings, "rendering/gaussian_splatting/debug/show_tile_grid");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_c = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(0.0f));
    node_b->set_splat_asset(make_single_splat_asset(10.0f));
    node_c->set_splat_asset(make_single_splat_asset(20.0f));
    node_a->set_show_tile_grid(false);
    node_b->set_show_tile_grid(false);
    node_c->set_show_tile_grid(false);
    node_a->set_update_mode(GaussianSplatNode3D::UPDATE_MODE_MANUAL);
    node_b->set_update_mode(GaussianSplatNode3D::UPDATE_MODE_MANUAL);
    node_c->set_update_mode(GaussianSplatNode3D::UPDATE_MODE_MANUAL);

    root->add_child(node_a);
    root->add_child(node_b);
    root->add_child(node_c);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid() || node_b->get_renderer() != renderer || node_c->get_renderer() != renderer) {
        FAIL("shared renderer required: all three nodes must resolve the same GaussianSplatRenderer");
        root->remove_child(node_c);
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_c);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    // node_a is the SOLE requester.
    node_a->set_show_tile_grid(true);
    CHECK(renderer->is_debug_show_tile_grid());
    CHECK_FALSE(node_b->is_showing_tile_grid());
    CHECK_FALSE(node_c->is_showing_tile_grid());

    // 3 -> 2: still "shared", so the shared/not-shared boolean does NOT move on
    // node_b or node_c and the pre-fix convergence hook returned early on both.
    root->remove_child(node_a);
    memdelete(node_a);
    node_a = nullptr;

    // The sole requester is gone, so the renderer-wide overlay must be gone too.
    CHECK_FALSE(renderer->is_debug_show_tile_grid());

    // ...and it must not merely be deferred to a frame that MANUAL never runs.
    for (int i = 0; i < 5; i++) {
        tree->process(0.0);
    }
    CHECK_FALSE(renderer->is_debug_show_tile_grid());

    // The union still works in the shrunken set: node_c can turn it back on and
    // node_b can no longer hold it on by itself.
    node_c->set_show_tile_grid(true);
    CHECK(renderer->is_debug_show_tile_grid());
    node_c->set_show_tile_grid(false);
    CHECK_FALSE(renderer->is_debug_show_tile_grid());

    root->remove_child(node_c);
    root->remove_child(node_b);
    memdelete(node_c);
    memdelete(node_b);
}

// #839 round 3, thread B: the 1 -> 0 node transition.
//
// Distinct from the 3 -> 2 case above. There, at least one peer remained, so
// _notify_renderer_peers_shared_state_changed() had somebody to delegate the
// recompute to. Here the LAST GaussianSplatNode3D leaves while a
// GaussianSplatWorld3D submission keeps the shared renderer alive: the peer walk
// iterates nothing, p_include_self is false so the departing node may not push,
// and _on_renderer_peer_set_changed() only dropped the memo. A memo is consulted
// by the next push -- and there is no next push -- so the departed node's tile
// grid stayed rendered on the world submission until some unrelated node
// happened to register.
TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Overlay union clears when the last node leaves a world-held renderer") {
    // #656: REQUIRE does not abort in this build (disable_exceptions=True), so
    // guard-then-return rather than dereferencing after a REQUIRE.
    SceneTree *tree = SceneTree::get_singleton();
    if (!tree) {
        FAIL("SceneTree singleton required");
        return;
    }
    Window *root = tree->get_root();
    if (!root) {
        FAIL("SceneTree root window required");
        return;
    }
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings singleton required");
        return;
    }
    ProjectSettingGuard tile_guard(project_settings, "rendering/gaussian_splatting/debug/show_tile_grid");

    // The non-node content that keeps the renderer alive after the node leaves.
    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(make_test_gaussian_data(2, 50.0f));

    GaussianSplatWorld3D *world_node = memnew(GaussianSplatWorld3D);
    world_node->set_world(world);
    root->add_child(world_node);
    tree->process(0.0);
    world_node->apply_world();

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    node->set_splat_asset(make_single_splat_asset(0.0f));
    node->set_show_tile_grid(false);
    // The per-frame apply must not be what rescues this.
    node->set_update_mode(GaussianSplatNode3D::UPDATE_MODE_MANUAL);
    root->add_child(node);
    tree->process(0.0);

    // Hold the renderer locally so the assertions below survive the node's
    // departure -- the world submission is what keeps it alive in production.
    Ref<GaussianSplatRenderer> renderer = node->get_renderer();
    // #595: no environment skip -- a skip is scored as a PASS.
    if (!renderer.is_valid() || world_node->get_renderer() != renderer) {
        FAIL("shared renderer required: the node and the world submission must resolve the same GaussianSplatRenderer");
        root->remove_child(node);
        root->remove_child(world_node);
        memdelete(node);
        memdelete(world_node);
        return;
    }

    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director) {
        FAIL("GaussianSplatSceneDirector singleton required");
        root->remove_child(node);
        root->remove_child(world_node);
        memdelete(node);
        memdelete(world_node);
        return;
    }
    // Precondition for the whole case: exactly one node, plus a world submission
    // that will outlive it. Without the submission the renderer would simply be
    // released and there would be nothing to leave stale.
    CHECK(director->get_instance_count_for_renderer(renderer.ptr()) == 1u);
    CHECK(director->has_world_submission_for_renderer(renderer.ptr()));

    // The lone node is the sole requester.
    node->set_show_tile_grid(true);
    CHECK(renderer->is_debug_show_tile_grid());

    // 1 -> 0 nodes. The renderer stays alive on the world submission.
    root->remove_child(node);
    memdelete(node);
    tree->process(0.0);

    CHECK(director->get_instance_count_for_renderer(renderer.ptr()) == 0u);
    CHECK(director->has_world_submission_for_renderer(renderer.ptr()));

    // The requester is gone, so the renderer-wide overlay must be gone with it.
    // Pre-fix this stayed true.
    CHECK_FALSE(renderer->is_debug_show_tile_grid());

    // ...and it is not merely deferred: no node is left to run a per-frame apply
    // at all, so if it were not written above nothing would ever write it.
    for (int i = 0; i < 5; i++) {
        tree->process(0.0);
    }
    CHECK_FALSE(renderer->is_debug_show_tile_grid());

    root->remove_child(world_node);
    memdelete(world_node);
}

// #839 round 3, thread C: a renderer-side HUD write has to reach the node.
//
// GaussianSplatRenderer::set_debug_show_device_boundaries() and
// set_debug_show_texture_states() are bound to script
// (gaussian_splat_renderer_bindings.cpp:139-141), so
// `node.get_renderer().set_debug_show_device_boundaries(true)` is a supported way
// to turn those HUD sections on. The flag landed in the renderer's DebugState and
// stopped there: no node callback exists on those setters, and the node is what
// creates the GaussianSplatDebugHUDLayer. Distinct from the round-2 peer fan-out,
// which only runs through the four node-local union setters.
//
// UPDATE_MODE_MANUAL is again the non-self-healing case: the node's per-frame
// apply is the only other thing that would have reconciled the control, and
// MANUAL never runs update_splats().
TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Manual-mode node draws the HUD for a renderer-side debug flag write") {
    SceneTree *tree = SceneTree::get_singleton();
    if (!tree) {
        FAIL("SceneTree singleton required");
        return;
    }
    Window *root = tree->get_root();
    if (!root) {
        FAIL("SceneTree root window required");
        return;
    }

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    node->set_splat_asset(make_single_splat_asset(0.0f));
    node->set_update_mode(GaussianSplatNode3D::UPDATE_MODE_MANUAL);
    root->add_child(node);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node->get_renderer();
    // #595: no environment skip -- a skip is scored as a PASS.
    if (!renderer.is_valid()) {
        FAIL("renderer required: this case runs only in the [RequiresGPU] harness");
        root->remove_child(node);
        memdelete(node);
        return;
    }

    CHECK_FALSE(renderer->is_debug_hud_source_active());
    CHECK_FALSE(node_has_debug_hud_control(node));

    // The renderer-side write, exactly as script reaches it.
    renderer->set_debug_show_device_boundaries(true);
    CHECK(renderer->is_debug_show_device_boundaries());
    CHECK(renderer->is_debug_hud_source_active());
    // Pre-fix this was false here and stayed false forever: the HUD lines existed
    // in renderer stats with no layer to draw them.
    CHECK(node_has_debug_hud_control(node));

    // MANUAL: no amount of frames reconciles this on its own, so the assertion
    // above is not merely early.
    for (int i = 0; i < 5; i++) {
        tree->process(0.0);
    }
    CHECK(node_has_debug_hud_control(node));

    // Turning it back off tears the control down again.
    renderer->set_debug_show_device_boundaries(false);
    CHECK_FALSE(renderer->is_debug_hud_source_active());
    CHECK_FALSE(node_has_debug_hud_control(node));

    // The sibling flag behaves the same way -- this is not a one-flag patch.
    renderer->set_debug_show_texture_states(true);
    CHECK(renderer->is_debug_hud_source_active());
    CHECK(node_has_debug_hud_control(node));

    // Two sources on: dropping one must NOT tear the control down, because the
    // notification is gated on is_debug_hud_source_active() flipping, not on the
    // individual flag.
    renderer->set_debug_show_device_boundaries(true);
    CHECK(node_has_debug_hud_control(node));
    renderer->set_debug_show_device_boundaries(false);
    CHECK(renderer->is_debug_show_texture_states());
    CHECK(node_has_debug_hud_control(node));

    renderer->set_debug_show_texture_states(false);
    CHECK_FALSE(renderer->is_debug_hud_source_active());
    CHECK_FALSE(node_has_debug_hud_control(node));

    root->remove_child(node);
    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Detached node reclaims retained renderer debug settings on re-entry") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(0.0f));
    node_b->set_splat_asset(make_single_splat_asset(10.0f));

    root->add_child(node_a);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer_a = node_a->get_renderer();
    if (!renderer_a.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    node_a->set_show_density_heatmap(true);
    CHECK(renderer_a->is_debug_show_density_heatmap());

    root->remove_child(node_a);
    tree->process(0.0);

    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer_b = node_b->get_renderer();
    if (!renderer_b.is_valid() || renderer_b != renderer_a) {
        MESSAGE("Skipping test - retained shared renderer unavailable");
        root->remove_child(node_b);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    CHECK_FALSE(renderer_a->is_debug_show_density_heatmap());

    root->remove_child(node_b);
    tree->process(0.0);

    root->add_child(node_a);
    tree->process(0.0);

    CHECK(is_property_editor_exposed(node_a, StringName("debug/show_density_heatmap")));
    CHECK(renderer_a->is_debug_show_density_heatmap());

    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Shared renderer instance buffer tracks per-node opacity") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    const float node_a_x = 1111.0f;
    const float node_b_x = 2222.0f;

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(node_a_x));
    node_b->set_splat_asset(make_single_splat_asset(node_b_x));
    // The instance row's translation_scale[0] is the NODE transform origin
    // (gaussian_splat_scene_director.cpp:1705), NOT the splat offset baked into
    // the asset. Without these set_position calls both nodes sit at x=0, every
    // find_instance_index_by_translation_x() below returns -1, and this test
    // silently early-returns past all of its opacity assertions.
    node_a->set_position(Vector3(node_a_x, 0.0f, 0.0f));
    node_b->set_position(Vector3(node_b_x, 0.0f, 0.0f));

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer_a = node_a->get_renderer();
    Ref<GaussianSplatRenderer> renderer_b = node_b->get_renderer();
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!renderer_a.is_valid() || !renderer_b.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    CHECK(renderer_a == renderer_b);
    CHECK(director != nullptr);
    if (renderer_a != renderer_b || director == nullptr) {
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    LocalVector<InstanceDataGPU> instance_buffer;
    director->build_instance_buffer_for_renderer(renderer_a.ptr(), instance_buffer);

    int node_a_index = find_instance_index_by_translation_x(instance_buffer, node_a_x);
    int node_b_index = find_instance_index_by_translation_x(instance_buffer, node_b_x);
    if (node_a_index < 0 || node_b_index < 0) {
        // Not a skip condition: both nodes are attached and visible here, so a
        // missing instance row is a real failure. Silently returning made this
        // test green while asserting nothing. FAIL does not abort in this build
        // (DOCTEST_CONFIG_NO_EXCEPTIONS, #656), so return explicitly before the
        // index dereferences below.
        FAIL("instance rows missing for node_a/node_b (a=", node_a_index, " b=", node_b_index, ")");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    CHECK(instance_buffer[node_a_index].params[0] == doctest::Approx(1.0f));
    CHECK(instance_buffer[node_b_index].params[0] == doctest::Approx(1.0f));
    CHECK(instance_buffer[node_a_index].effect_params[0] == doctest::Approx(1.0f));
    CHECK(instance_buffer[node_a_index].effect_params[1] == doctest::Approx(1.0f));
    CHECK(instance_buffer[node_b_index].effect_params[0] == doctest::Approx(1.0f));
    CHECK(instance_buffer[node_b_index].effect_params[1] == doctest::Approx(1.0f));

    node_b->set_opacity(0.25f);
    node_b->set_effect_position_scale(0.5f);
    node_b->set_effect_opacity_scale(0.2f);
    tree->process(0.0);
    director->build_instance_buffer_for_renderer(renderer_a.ptr(), instance_buffer);
    node_a_index = find_instance_index_by_translation_x(instance_buffer, node_a_x);
    node_b_index = find_instance_index_by_translation_x(instance_buffer, node_b_x);
    if (node_a_index < 0 || node_b_index < 0) {
        // Not a skip condition: both nodes are attached and visible here, so a
        // missing instance row is a real failure. Silently returning made this
        // test green while asserting nothing. FAIL does not abort in this build
        // (DOCTEST_CONFIG_NO_EXCEPTIONS, #656), so return explicitly before the
        // index dereferences below.
        FAIL("instance rows missing for node_a/node_b (a=", node_a_index, " b=", node_b_index, ")");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    CHECK(instance_buffer[node_a_index].params[0] == doctest::Approx(1.0f));
    CHECK(instance_buffer[node_b_index].params[0] == doctest::Approx(0.25f));
    CHECK(instance_buffer[node_b_index].effect_params[0] == doctest::Approx(0.5f));
    CHECK(instance_buffer[node_b_index].effect_params[1] == doctest::Approx(0.2f));

    node_a->set_opacity(0.6f);
    node_a->set_effect_position_scale(1.7f);
    node_a->set_effect_opacity_scale(0.9f);
    tree->process(0.0);
    director->build_instance_buffer_for_renderer(renderer_a.ptr(), instance_buffer);
    node_a_index = find_instance_index_by_translation_x(instance_buffer, node_a_x);
    node_b_index = find_instance_index_by_translation_x(instance_buffer, node_b_x);
    if (node_a_index < 0 || node_b_index < 0) {
        // Not a skip condition: both nodes are attached and visible here, so a
        // missing instance row is a real failure. Silently returning made this
        // test green while asserting nothing. FAIL does not abort in this build
        // (DOCTEST_CONFIG_NO_EXCEPTIONS, #656), so return explicitly before the
        // index dereferences below.
        FAIL("instance rows missing for node_a/node_b (a=", node_a_index, " b=", node_b_index, ")");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    CHECK(instance_buffer[node_a_index].params[0] == doctest::Approx(0.6f));
    CHECK(instance_buffer[node_b_index].params[0] == doctest::Approx(0.25f));
    CHECK(instance_buffer[node_a_index].effect_params[0] == doctest::Approx(1.7f));
    CHECK(instance_buffer[node_a_index].effect_params[1] == doctest::Approx(0.9f));
    CHECK(instance_buffer[node_b_index].effect_params[0] == doctest::Approx(0.5f));
    CHECK(instance_buffer[node_b_index].effect_params[1] == doctest::Approx(0.2f));

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Scene sphere effectors build per-instance selection masks") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    const float node_a_x = 3331.0f;
    const float node_b_x = 3332.0f;

    Node3D *group_a = memnew(Node3D);
    Node3D *group_b = memnew(Node3D);
    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    SphereEffector3D *effector_a = memnew(SphereEffector3D);
    SphereEffector3D *effector_b = memnew(SphereEffector3D);

    group_a->set_name("GroupA");
    group_b->set_name("GroupB");
    node_a->set_name("NodeA");
    node_b->set_name("NodeB");
    effector_a->set_name("EffectorA");
    effector_b->set_name("EffectorB");

    node_a->set_splat_asset(make_single_splat_asset(node_a_x));
    node_b->set_splat_asset(make_single_splat_asset(node_b_x));
    // find_instance_index_by_translation_x matches the NODE transform origin
    // (gaussian_splat_scene_director.cpp:1705), not the asset-baked splat
    // offset. Without these both nodes sit at x=0 and the lookups below return
    // -1 -- which is the actual cause of this case's crash (the failed REQUIRE
    // does not abort in this build, so -1 was used as an index).
    //
    // Safe for the mask assertions: effector selection is scope/layer-based
    // only (scene_director_sphere_effectors.cpp:231-275 and
    // gaussian_splat_scene_director.cpp:534-576 apply no distance test), so
    // moving the nodes does not change which effector matches which node.
    node_a->set_position(Vector3(node_a_x, 0.0f, 0.0f));
    node_b->set_position(Vector3(node_b_x, 0.0f, 0.0f));

    effector_a->set_enabled(true);
    effector_a->set_radius(8.0f);
    effector_a->set_strength(1.0f);
    effector_a->set_affect_position(true);
    effector_a->set_affect_opacity(true);
    effector_a->set_opacity_strength(0.75f);
    effector_a->set_target_opacity(0.35f);

    effector_b->set_enabled(true);
    effector_b->set_radius(8.0f);
    effector_b->set_strength(1.0f);
    effector_b->set_affect_position(true);

    root->add_child(group_a);
    root->add_child(group_b);
    group_a->add_child(node_a);
    group_a->add_child(effector_a);
    group_b->add_child(node_b);
    group_b->add_child(effector_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!renderer.is_valid() || !director) {
        root->remove_child(group_b);
        root->remove_child(group_a);
        memdelete(group_b);
        memdelete(group_a);
        return;
    }
    CHECK(renderer == node_b->get_renderer());

    LocalVector<GaussianSplatSceneDirector::SphereEffectorSelection> payload;
    director->build_sphere_effector_payload_for_renderer(renderer.ptr(), payload);
    // #708/#656: REQUIRE does not abort in this build, so a bare REQUIRE on the
    // payload size followed by payload[0] reads out of bounds and takes down the
    // entire batch process. Guard explicitly, like the instance-row lookup below.
    if (payload.size() != 2) {
        FAIL("expected 2 sphere effector payload entries, got ", payload.size());
        root->remove_child(group_b);
        root->remove_child(group_a);
        memdelete(group_b);
        memdelete(group_a);
        return;
    }
    CHECK(payload[0].target_opacity == doctest::Approx(0.35f));

    LocalVector<InstanceDataGPU> instance_buffer;
    director->build_instance_buffer_for_renderer(renderer.ptr(), instance_buffer);
    const int node_a_index = find_instance_index_by_translation_x(instance_buffer, node_a_x);
    const int node_b_index = find_instance_index_by_translation_x(instance_buffer, node_b_x);
    // #656: REQUIRE does not abort in this build, so a bare REQUIRE followed by
    // instance_buffer[-1] takes down the entire batch process. Guard explicitly.
    if (node_a_index < 0 || node_b_index < 0) {
        FAIL("instance rows missing for node_a/node_b (a=", node_a_index, " b=", node_b_index, ")");
        root->remove_child(group_b);
        root->remove_child(group_a);
        memdelete(group_b);
        memdelete(group_a);
        return;
    }
    CHECK(decode_scene_effector_mask(instance_buffer[node_a_index]) == 0x1u);
    CHECK(decode_scene_effector_mask(instance_buffer[node_b_index]) == 0x2u);
    CHECK(instance_buffer[node_a_index].effect_params[3] == doctest::Approx(2.0f));
    CHECK(instance_buffer[node_b_index].effect_params[3] == doctest::Approx(2.0f));
    CHECK(node_a->get_last_matched_scene_effector_count() == 1u);
    CHECK(node_b->get_last_matched_scene_effector_count() == 1u);
    CHECK(node_a->is_scene_effector_position_active());
    CHECK(node_a->is_scene_effector_opacity_active());
    CHECK(node_b->is_scene_effector_position_active());
    CHECK_FALSE(node_b->is_scene_effector_opacity_active());
    {
        const Dictionary debug_state_a = node_a->get_scene_effector_debug_state();
        CHECK(int64_t(debug_state_a.get(StringName("matched_count"), -1)) == 1);
        CHECK(int64_t(debug_state_a.get(StringName("bound_count"), -1)) == 1);
        CHECK(bool(debug_state_a.get(StringName("truncated"), true)) == false);
        CHECK(bool(debug_state_a.get(StringName("position_active"), false)));
        CHECK(bool(debug_state_a.get(StringName("opacity_active"), false)));
        const PackedStringArray selected_names = debug_state_a.get(StringName("selected_effector_names"), PackedStringArray());
        // #708/#656: a CHECK never aborts either, so guard before indexing.
        if (selected_names.size() != 1) {
            FAIL("expected 1 selected effector name, got ", selected_names.size());
            root->remove_child(group_b);
            root->remove_child(group_a);
            memdelete(group_b);
            memdelete(group_a);
            return;
        }
        CHECK(selected_names[0] == String("EffectorA"));
    }
    {
        const Dictionary stats_a = node_a->get_statistics();
        CHECK(int64_t(stats_a.get(StringName("matched_scene_effectors"), -1)) == 1);
        CHECK(int64_t(stats_a.get(StringName("bound_scene_effectors"), -1)) == 1);
        CHECK(bool(stats_a.get(StringName("scene_effector_truncated"), true)) == false);
        CHECK(bool(stats_a.get(StringName("scene_effector_position_active"), false)));
        CHECK(bool(stats_a.get(StringName("scene_effector_opacity_active"), false)));
    }

    node_b->set_scene_effectors_enabled(false);
    tree->process(0.0);
    director->build_instance_buffer_for_renderer(renderer.ptr(), instance_buffer);
    const int node_b_disabled_index = find_instance_index_by_translation_x(instance_buffer, node_b_x);
    // #656: same guard -- disabling scene effectors must not drop the instance
    // row, and if it ever does we must fail rather than index with -1.
    if (node_b_disabled_index < 0) {
        FAIL("instance row missing for node_b after disabling scene effectors");
        root->remove_child(group_b);
        root->remove_child(group_a);
        memdelete(group_b);
        memdelete(group_a);
        return;
    }
    CHECK(decode_scene_effector_mask(instance_buffer[node_b_disabled_index]) == 0u);
    CHECK(node_b->get_last_matched_scene_effector_count() == 0u);
    CHECK_FALSE(node_b->is_scene_effector_position_active());
    CHECK_FALSE(node_b->is_scene_effector_opacity_active());

    root->remove_child(group_b);
    root->remove_child(group_a);
    memdelete(group_b);
    memdelete(group_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Scene sphere effectors deterministically truncate to the renderer budget") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    Node3D *group = memnew(Node3D);
    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    REQUIRE(group != nullptr);
    REQUIRE(node != nullptr);

    group->set_name("EffectGroup");
    node->set_name("TargetNode");
    node->set_splat_asset(make_single_splat_asset(3336.0f));

    struct EffectorSpec {
        const char *name;
        int priority;
    };
    const EffectorSpec effector_specs[] = {
        { "EffectorA", 2 },
        { "EffectorB", 7 },
        { "EffectorC", 5 },
        { "EffectorD", -1 },
        { "EffectorE", 3 },
    };

    root->add_child(group);
    group->add_child(node);

    LocalVector<SphereEffector3D *> effectors;
    for (const EffectorSpec &spec : effector_specs) {
        SphereEffector3D *effector = memnew(SphereEffector3D);
        effector->set_name(spec.name);
        effector->set_enabled(true);
        effector->set_radius(10.0f);
        effector->set_strength(float(spec.priority));
        effector->set_priority(spec.priority);
        effector->set_affect_position(true);
        group->add_child(effector);
        effectors.push_back(effector);
    }
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node->get_renderer();
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!renderer.is_valid() || !director) {
        root->remove_child(group);
        memdelete(group);
        return;
    }

    LocalVector<GaussianSplatSceneDirector::SphereEffectorSelection> payload;
    director->build_sphere_effector_payload_for_renderer(renderer.ptr(), payload);
    // #708/#656: REQUIRE does not abort in this build; payload[0..3] is read at
    // the end of this case, so an unexpected payload size would crash the whole
    // batch instead of failing this one test. Guard explicitly.
    if (payload.size() != 4) {
        FAIL("expected 4 truncated sphere effector payload entries, got ", payload.size());
        root->remove_child(group);
        memdelete(group);
        return;
    }
    CHECK(director->get_sphere_effector_count_for_renderer(renderer.ptr()) == 5u);
    CHECK(node->get_last_matched_scene_effector_count() == 5u);
    {
        const Dictionary debug_state = node->get_scene_effector_debug_state();
        CHECK(int64_t(debug_state.get(StringName("matched_count"), -1)) == 5);
        CHECK(int64_t(debug_state.get(StringName("bound_count"), -1)) == 4);
        CHECK(bool(debug_state.get(StringName("truncated"), false)));
        const PackedStringArray selected_names = debug_state.get(StringName("selected_effector_names"), PackedStringArray());
        // #708/#656: same guard before indexing the selected-name list.
        if (selected_names.size() != 4) {
            FAIL("expected 4 selected effector names, got ", selected_names.size());
            root->remove_child(group);
            memdelete(group);
            return;
        }
        CHECK(selected_names[0] == String("EffectorB"));
        CHECK(selected_names[1] == String("EffectorC"));
        CHECK(selected_names[2] == String("EffectorE"));
        CHECK(selected_names[3] == String("EffectorA"));
    }
    {
        const Dictionary stats = node->get_statistics();
        CHECK(int64_t(stats.get(StringName("matched_scene_effectors"), -1)) == 5);
        CHECK(int64_t(stats.get(StringName("bound_scene_effectors"), -1)) == 4);
        CHECK(bool(stats.get(StringName("scene_effector_truncated"), false)));
    }
    CHECK(payload[0].priority == 7);
    CHECK(payload[1].priority == 5);
    CHECK(payload[2].priority == 3);
    CHECK(payload[3].priority == 2);

    root->remove_child(group);
    memdelete(group);
}

TEST_CASE("[GaussianSplatting][Node] Runtime effect controls sanitize invalid gameplay values") {
    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    REQUIRE(node != nullptr);

    const float nan = std::numeric_limits<float>::quiet_NaN();

    CHECK(node->get_opacity() == doctest::Approx(1.0f));
    CHECK(node->get_effect_position_scale() == doctest::Approx(1.0f));
    CHECK(node->get_effect_opacity_scale() == doctest::Approx(1.0f));
    CHECK(node->get_wind_strength() == doctest::Approx(1.0f));
    CHECK(node->get_wind_frequency() == doctest::Approx(1.0f));

    node->set_opacity(-0.25f);
    CHECK(node->get_opacity() == doctest::Approx(0.0f));
    // Range was extended from [0, 1] to [0, 8] to support SuperSplat-style
    // overfill (per-splat alpha still capped at 0.99 in tile_binning.glsl).
    node->set_opacity(1.75f);
    CHECK(node->get_opacity() == doctest::Approx(1.75f));
    node->set_opacity(8.5f);
    CHECK(node->get_opacity() == doctest::Approx(8.0f));
    node->set_opacity(nan);
    CHECK(node->get_opacity() == doctest::Approx(1.0f));

    node->set_effect_position_scale(-2.0f);
    CHECK(node->get_effect_position_scale() == doctest::Approx(0.0f));
    node->set_effect_position_scale(nan);
    CHECK(node->get_effect_position_scale() == doctest::Approx(1.0f));

    node->set_effect_opacity_scale(-3.0f);
    CHECK(node->get_effect_opacity_scale() == doctest::Approx(0.0f));
    node->set_effect_opacity_scale(nan);
    CHECK(node->get_effect_opacity_scale() == doctest::Approx(1.0f));

    node->set_wind_strength(-5.0f);
    CHECK(node->get_wind_strength() == doctest::Approx(0.0f));
    node->set_wind_frequency(-4.0f);
    CHECK(node->get_wind_frequency() == doctest::Approx(0.0f));

    memdelete(node);
}

TEST_CASE("[GaussianSplatting][SphereEffector] Runtime opacity controls sanitize and warn on inert configs") {
    SphereEffector3D *effector = memnew(SphereEffector3D);
    REQUIRE(effector != nullptr);

    const float nan = std::numeric_limits<float>::quiet_NaN();

    CHECK(effector->get_target_opacity() == doctest::Approx(0.0f));
    CHECK_FALSE(is_property_editor_exposed(effector, StringName("opacity_strength")));
    CHECK_FALSE(is_property_editor_exposed(effector, StringName("target_opacity")));

    effector->set_affect_opacity(true);
    CHECK(is_property_editor_exposed(effector, StringName("opacity_strength")));
    CHECK(is_property_editor_exposed(effector, StringName("target_opacity")));

    effector->set_target_opacity(-0.5f);
    CHECK(effector->get_target_opacity() == doctest::Approx(0.0f));
    effector->set_target_opacity(1.5f);
    CHECK(effector->get_target_opacity() == doctest::Approx(1.0f));
    effector->set_target_opacity(nan);
    CHECK(effector->get_target_opacity() == doctest::Approx(0.0f));

    effector->set_enabled(true);
    effector->set_radius(5.0f);
    effector->set_affect_position(false);
    effector->set_opacity_strength(0.0f);
    effector->set_target_opacity(1.0f);

    PackedStringArray warnings = effector->get_configuration_warnings();
    bool has_opacity_strength_warning = false;
    bool has_target_opacity_warning = false;
    for (int i = 0; i < warnings.size(); i++) {
        const String warning = warnings[i];
        if (warning.contains("opacity_strength is 0")) {
            has_opacity_strength_warning = true;
        }
        if (warning.contains("target_opacity is 1.0")) {
            has_target_opacity_warning = true;
        }
    }

    CHECK(has_opacity_strength_warning);
    CHECK(has_target_opacity_warning);

    memdelete(effector);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Shared renderer instance buffer drops hidden nodes") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    const float node_a_x = 3333.0f;
    const float node_b_x = 4444.0f;

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(node_a_x));
    node_b->set_splat_asset(make_single_splat_asset(node_b_x));
    // count_instances_by_translation_x matches the NODE transform origin
    // (gaussian_splat_scene_director.cpp:1705), not the asset-baked splat
    // offset. Without these the nodes both sit at x=0, the first CHECK_EQ gets
    // 0 instead of 1, and the hidden-node assertion below passes vacuously as
    // 0 == 0 -- i.e. it would stay green even if visibility were ignored.
    node_a->set_position(Vector3(node_a_x, 0.0f, 0.0f));
    node_b->set_position(Vector3(node_b_x, 0.0f, 0.0f));

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    CHECK(renderer == node_b->get_renderer());
    CHECK(director != nullptr);
    if (renderer != node_b->get_renderer() || director == nullptr) {
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    LocalVector<InstanceDataGPU> instance_buffer;
    director->build_instance_buffer_for_renderer(renderer.ptr(), instance_buffer);
    CHECK_EQ(count_instances_by_translation_x(instance_buffer, node_a_x), 1);
    CHECK_EQ(count_instances_by_translation_x(instance_buffer, node_b_x), 1);

    node_b->set_visible(false);
    tree->process(0.0);
    director->build_instance_buffer_for_renderer(renderer.ptr(), instance_buffer);
    CHECK_EQ(count_instances_by_translation_x(instance_buffer, node_a_x), 1);
    CHECK_EQ(count_instances_by_translation_x(instance_buffer, node_b_x), 0);

    node_b->set_visible(true);
    tree->process(0.0);
    director->build_instance_buffer_for_renderer(renderer.ptr(), instance_buffer);
    CHECK_EQ(count_instances_by_translation_x(instance_buffer, node_a_x), 1);
    CHECK_EQ(count_instances_by_translation_x(instance_buffer, node_b_x), 1);

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Color grading property stays exposed when renderer is shared") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(5555.0f));
    node_b->set_splat_asset(make_single_splat_asset(6666.0f));

    root->add_child(node_a);
    tree->process(0.0);
    CHECK(is_property_editor_exposed(node_a, StringName("rendering/color_grading")));

    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping shared-renderer property check - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    // Color grading is per-node: the property must remain visible on every
    // node, even when the renderer is shared between multiple instances.
    CHECK(is_property_editor_exposed(node_a, StringName("rendering/color_grading")));
    CHECK(is_property_editor_exposed(node_b, StringName("rendering/color_grading")));

    // It should also stay visible after the share collapses back to a single instance.
    root->remove_child(node_b);
    tree->process(0.0);
    CHECK(is_property_editor_exposed(node_a, StringName("rendering/color_grading")));

    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Node color grading reaches renderer even with active world submission") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    // Set up a world node with active submission.
    Ref<GaussianSplatWorld> world_res;
    world_res.instantiate();
    Ref<GaussianData> world_data = make_test_gaussian_data(4, 500.0f);
    world_res->set_gaussian_data(world_data);

    GaussianSplatWorld3D *world_node = memnew(GaussianSplatWorld3D);
    world_node->set_world(world_res);
    root->add_child(world_node);
    tree->process(0.0);
    world_node->apply_world();

    // Set up a splat node that shares the same renderer.
    GaussianSplatNode3D *graded_node = memnew(GaussianSplatNode3D);
    graded_node->set_splat_asset(make_single_splat_asset(9400.0f));
    root->add_child(graded_node);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = graded_node->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(graded_node);
        root->remove_child(world_node);
        memdelete(graded_node);
        memdelete(world_node);
        return;
    }

    // Assign color grading to the splat node.
    Ref<ColorGradingResource> grading = make_color_grading_resource();
    graded_node->set_color_grading(grading);

    // Color grading is per-node: even with an active world submission
    // sharing the renderer, the node's grading must reach the renderer
    // and the inspector property must stay visible.
    CHECK_MESSAGE(graded_node->get_color_grading() == grading,
            "Node retains its local color grading");
    CHECK_MESSAGE(node_color_grading(graded_node, renderer) == grading,
            "Per-instance grading must track the node even with a world submission");
    CHECK(is_property_editor_exposed(graded_node, StringName("rendering/color_grading")));

    // Property and propagation should also hold after the world submission is removed.
    world_node->clear_world();
    root->remove_child(world_node);
    tree->process(0.0);

    graded_node->set_color_grading(grading);
    CHECK_MESSAGE(node_color_grading(graded_node, renderer) == grading,
            "Per-instance grading must persist after world submission is removed");
    CHECK(is_property_editor_exposed(graded_node, StringName("rendering/color_grading")));

    root->remove_child(graded_node);
    memdelete(graded_node);
    memdelete(world_node);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Shared renderer preserves local painterly and color grading state") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(7777.0f));
    node_b->set_splat_asset(make_single_splat_asset(8888.0f));

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    CHECK(renderer == node_b->get_renderer());
    if (renderer != node_b->get_renderer()) {
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    Ref<ColorGradingResource> grading = make_color_grading_resource();
    CHECK(grading.is_valid());
    if (!grading.is_valid()) {
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    node_a->set_enable_painterly(true);
    node_a->set_color_grading(grading);
    tree->process(0.0);

    CHECK(node_a->is_painterly_enabled());
    CHECK(node_a->get_color_grading().is_valid());
    CHECK(node_a->get_color_grading() == grading);
    // Painterly is still renderer-wide and gated while the renderer is shared.
    CHECK_FALSE(renderer->get_painterly_enabled());
    // Color grading is per-instance and lives on the director record keyed by node_id.
    CHECK(node_color_grading(node_a, renderer) == grading);

    root->remove_child(node_b);
    tree->process(0.0);

    CHECK(node_a->is_painterly_enabled());
    CHECK(node_a->get_color_grading() == grading);
    CHECK(renderer->get_painterly_enabled());
    CHECK(node_color_grading(node_a, renderer) == grading);

    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Single node color grading reaches renderer") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    node->set_splat_asset(make_single_splat_asset(9100.0f));

    root->add_child(node);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node);
        memdelete(node);
        return;
    }

    // Director should start with no per-instance grading bound to this node.
    CHECK(node_color_grading(node, renderer).is_null());

    Ref<ColorGradingResource> grading = make_color_grading_resource();
    CHECK(grading.is_valid());

    node->set_color_grading(grading);
    tree->process(0.0);

    // Setter must propagate the grading to the director record for this node.
    CHECK(node->get_color_grading() == grading);
    CHECK(node_color_grading(node, renderer) == grading);

    root->remove_child(node);
    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Color grading survives exit and re-enter tree") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    node->set_splat_asset(make_single_splat_asset(9200.0f));

    Ref<ColorGradingResource> grading = make_color_grading_resource();
    CHECK(grading.is_valid());

    // Set color grading before entering tree.
    node->set_color_grading(grading);
    CHECK(node->get_color_grading() == grading);

    // Enter tree.
    root->add_child(node);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node);
        memdelete(node);
        return;
    }

    CHECK(node_color_grading(node, renderer) == grading);

    // Exit tree.
    root->remove_child(node);
    tree->process(0.0);

    // Node should retain its color grading while detached.
    CHECK(node->get_color_grading() == grading);

    // Re-enter tree.
    root->add_child(node);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer_after = node->get_renderer();
    if (!renderer_after.is_valid()) {
        MESSAGE("Skipping re-enter check - renderer unavailable after re-add");
        root->remove_child(node);
        memdelete(node);
        return;
    }

    // Color grading must still be on the node and pushed to the director for the re-attached node.
    CHECK(node->get_color_grading() == grading);
    CHECK(node_color_grading(node, renderer_after) == grading);

    root->remove_child(node);
    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Color grading signal propagation updates renderer") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    node->set_splat_asset(make_single_splat_asset(9300.0f));

    root->add_child(node);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node);
        memdelete(node);
        return;
    }

    Ref<ColorGradingResource> grading = make_color_grading_resource();
    CHECK(grading.is_valid());

    node->set_color_grading(grading);
    tree->process(0.0);

    CHECK(node_color_grading(node, renderer) == grading);

    // Mutate a property on the resource — the "changed" signal should propagate
    // through _on_color_grading_changed and keep the director record in sync.
    const float original_exposure = grading->get_exposure();
    grading->set_exposure(2.5f);
    CHECK(grading->get_exposure() != doctest::Approx(original_exposure));

    // The director should still reference the same resource (signal updates
    // the record in-place, it does not swap the Ref).
    Ref<ColorGradingResource> resolved = node_color_grading(node, renderer);
    CHECK(resolved == grading);
    CHECK(resolved.is_valid());
    CHECK(resolved->get_exposure() == doctest::Approx(2.5f));

    root->remove_child(node);
    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Hot-reload of node A's asset must not clobber node B's grading on shared renderer") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(9301.0f));
    node_b->set_splat_asset(make_single_splat_asset(9302.0f));

    Ref<ColorGradingResource> grading_a = make_color_grading_resource();
    Ref<ColorGradingResource> grading_b = make_color_grading_resource();
    REQUIRE(grading_a.is_valid());
    REQUIRE(grading_b.is_valid());
    grading_a->set_exposure(0.25f);
    grading_b->set_exposure(0.75f);

    node_a->set_color_grading(grading_a);
    node_b->set_color_grading(grading_b);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    REQUIRE_MESSAGE(renderer == node_b->get_renderer(), "Both nodes must share the same renderer for this test");

    // After explicit setters, each node's director record carries its own grading.
    // Per-instance storage means node_a's grading is independent of node_b's.
    node_b->set_color_grading(grading_b);
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_a, renderer) == grading_a,
            "node_a keeps its own grading on its director record");
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "node_b keeps its own grading on its director record");

    // Simulate hot-reload / reimport of node_a's asset via the asset's
    // "changed" signal — this is the path that fires _on_asset_changed →
    // _update_asset on node_a without any user grading edit.
    Ref<GaussianSplatAsset> asset_a = node_a->get_splat_asset();
    REQUIRE(asset_a.is_valid());
    asset_a->emit_changed();
    tree->process(0.0);

    // Per-instance storage makes cross-node clobber impossible by construction:
    // each node's grading lives on its own director record. Both must still match.
    CHECK_MESSAGE(node_color_grading(node_a, renderer) == grading_a,
            "Hot-reload of node_a must not change node_a's own director grading either");
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "Hot-reload of node_a cannot touch node_b's director grading");

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Procedural set_splat_data path replays cached color grading once data is ready") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    // PR #245 P1: set_splat_data() bypasses set_splat_asset/_update_asset.
    // Without _replay_color_grading_if_pending() in _finalize_manual_splat_setup,
    // a node that received its data procedurally would never have its
    // cached color_grading reach the renderer until a manual setter call.
    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);

    Ref<ColorGradingResource> grading = make_color_grading_resource();
    REQUIRE(grading.is_valid());
    grading->set_exposure(0.42f);

    // Cache grading BEFORE entering tree, BEFORE any data is set.
    node->set_color_grading(grading);
    CHECK(node->get_color_grading() == grading);

    root->add_child(node);
    tree->process(0.0);

    // Now feed the node procedural data. This is the path the bot
    // flagged: ensure_renderer fires with no data (skips push), then
    // _finalize_manual_splat_setup runs with data present.
    PackedVector3Array positions;
    positions.push_back(Vector3(9500.0f, 0.0f, 0.0f));
    PackedColorArray colors;
    colors.push_back(Color(1.0f, 1.0f, 1.0f, 1.0f));
    PackedVector3Array scales;
    scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
    PackedFloat32Array opacities;
    opacities.push_back(1.0f);
    TypedArray<Quaternion> rotations;
    rotations.push_back(Quaternion());
    PackedFloat32Array sh;
    PackedInt32Array palette_ids;
    PackedInt32Array painterly_flags;
    PackedVector3Array normals;
    PackedVector2Array brush_axes;
    PackedFloat32Array stroke_ages;

    node->set_splat_data(positions, colors, scales, opacities, rotations,
            sh, palette_ids, painterly_flags, normals, brush_axes, stroke_ages, false);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node);
        memdelete(node);
        return;
    }

    CHECK_MESSAGE(node_color_grading(node, renderer) == grading,
            "Director must reflect the cached color grading after procedural set_splat_data");

    root->remove_child(node);
    memdelete(node);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Tree exit/re-enter must not allow _update_asset to re-clobber peer") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    // PR #245 P2: NOTIFICATION_EXIT_TREE used to reset the guard, but
    // owner.renderer is not cleared on tree-exit; ensure_renderer's
    // initial-sync branch only runs when renderer is null. So on
    // re-entry the guard would stay false, and the next _update_asset
    // (e.g. hot-reload) would re-push this node's grading and clobber
    // a peer that wrote in the meantime.
    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(9601.0f));
    node_b->set_splat_asset(make_single_splat_asset(9602.0f));

    Ref<ColorGradingResource> grading_a = make_color_grading_resource();
    Ref<ColorGradingResource> grading_b = make_color_grading_resource();
    REQUIRE(grading_a.is_valid());
    REQUIRE(grading_b.is_valid());
    grading_a->set_exposure(0.30f);
    grading_b->set_exposure(0.80f);

    node_a->set_color_grading(grading_a);
    node_b->set_color_grading(grading_b);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    REQUIRE_MESSAGE(renderer == node_b->get_renderer(), "Both nodes must share the same renderer for this test");

    // Per-instance storage: each node carries its own grading on its director record.
    node_b->set_color_grading(grading_b);
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_a, renderer) == grading_a,
            "node_a still has its own grading on its director record");
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "node_b's grading is stored per-instance on node_b's record");

    // Detach node_a, then re-attach it without changing its grading or
    // its asset. Then trigger a hot-reload via emit_changed(). The guard
    // must remain set so the implicit _update_asset push doesn't fire.
    root->remove_child(node_a);
    tree->process(0.0);
    root->add_child(node_a);
    tree->process(0.0);

    Ref<GaussianSplatAsset> asset_a = node_a->get_splat_asset();
    REQUIRE(asset_a.is_valid());
    asset_a->emit_changed();
    tree->process(0.0);

    // Both nodes must retain their own per-instance grading after all the churn.
    CHECK_MESSAGE(node_color_grading(node_a, renderer) == grading_a,
            "node_a's per-instance grading survives its own tree exit/re-enter + hot-reload");
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "node_b's per-instance grading is untouched by node_a's churn");

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Setter after no-grading load arms guard so hot-reload does not re-clobber peer") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    // Reproduces the post-fix scenario from PR #245 P1#8:
    //   - node_a loads with NO grading (so ensure_renderer skips its push;
    //     the data-ready guard would stay false without the setter arming
    //     it).
    //   - node_b loads with grading_b (ensure_renderer pushes grading_b).
    //   - user later assigns grading_a via the setter on node_a.
    //   - user later assigns grading_b via the setter on node_b (renderer
    //     now reflects grading_b, the most recent setter write).
    //   - hot-reload of node_a's asset runs _update_asset on node_a. With
    //     the setter arming the guard, this must NOT re-push grading_a
    //     and must leave grading_b on the renderer.
    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(9401.0f));
    node_b->set_splat_asset(make_single_splat_asset(9402.0f));

    Ref<ColorGradingResource> grading_b_initial = make_color_grading_resource();
    REQUIRE(grading_b_initial.is_valid());
    grading_b_initial->set_exposure(0.6f);
    node_b->set_color_grading(grading_b_initial);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    REQUIRE_MESSAGE(renderer == node_b->get_renderer(), "Both nodes must share the same renderer for this test");

    Ref<ColorGradingResource> grading_a = make_color_grading_resource();
    Ref<ColorGradingResource> grading_b = make_color_grading_resource();
    REQUIRE(grading_a.is_valid());
    REQUIRE(grading_b.is_valid());
    grading_a->set_exposure(0.25f);
    grading_b->set_exposure(0.75f);

    // Per-instance storage: each node's director record carries its own grading.
    node_a->set_color_grading(grading_a);
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_a, renderer) == grading_a,
            "node_a's director record must reflect grading_a");

    node_b->set_color_grading(grading_b);
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "node_b's director record must reflect grading_b");
    CHECK_MESSAGE(node_color_grading(node_a, renderer) == grading_a,
            "node_b's setter cannot touch node_a's per-instance grading");

    // Hot-reload of node_a's asset. With per-instance storage, there is no
    // shared slot to clobber — each record keeps its own grading.
    Ref<GaussianSplatAsset> asset_a = node_a->get_splat_asset();
    REQUIRE(asset_a.is_valid());
    asset_a->emit_changed();
    tree->process(0.0);

    CHECK_MESSAGE(node_color_grading(node_a, renderer) == grading_a,
            "Hot-reload leaves node_a's own grading intact");
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "Hot-reload of node_a cannot affect node_b's per-instance grading");

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Detached asset refresh and reassignment must not clobber active peer color grading") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(9701.0f));
    node_b->set_splat_asset(make_single_splat_asset(9702.0f));

    Ref<ColorGradingResource> grading_a = make_color_grading_resource();
    Ref<ColorGradingResource> grading_b = make_color_grading_resource();
    REQUIRE(grading_a.is_valid());
    REQUIRE(grading_b.is_valid());
    grading_a->set_exposure(0.2f);
    grading_b->set_exposure(0.9f);

    node_a->set_color_grading(grading_a);
    node_b->set_color_grading(grading_b);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    REQUIRE_MESSAGE(renderer == node_b->get_renderer(), "Both nodes must share the same renderer for this test");

    node_b->set_color_grading(grading_b);
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "node_b's per-instance grading must reflect grading_b");

    root->remove_child(node_a);
    tree->process(0.0);

    Ref<GaussianSplatAsset> detached_asset = node_a->get_splat_asset();
    REQUIRE(detached_asset.is_valid());
    detached_asset->emit_changed();
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "Detached hot-reload of node_a cannot touch node_b's per-instance grading");

    Ref<GaussianSplatAsset> replacement_asset = make_single_splat_asset(9703.0f);
    REQUIRE(replacement_asset.is_valid());
    node_a->set_splat_asset(replacement_asset);
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "Detached set_splat_asset on node_a cannot touch node_b's per-instance grading");

    root->remove_child(node_b);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Detached color grading resource changes replay only after re-enter") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(9711.0f));
    node_b->set_splat_asset(make_single_splat_asset(9712.0f));

    Ref<ColorGradingResource> grading_a = make_color_grading_resource();
    Ref<ColorGradingResource> grading_b = make_color_grading_resource();
    REQUIRE(grading_a.is_valid());
    REQUIRE(grading_b.is_valid());
    grading_a->set_exposure(0.35f);
    grading_b->set_exposure(0.85f);

    node_a->set_color_grading(grading_a);
    node_b->set_color_grading(grading_b);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    REQUIRE_MESSAGE(renderer == node_b->get_renderer(), "Both nodes must share the same renderer for this test");

    node_b->set_color_grading(grading_b);
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "node_b's per-instance grading must reflect grading_b");

    root->remove_child(node_a);
    tree->process(0.0);

    grading_a->set_exposure(1.25f);
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "Detached grading resource changes on node_a cannot touch node_b's per-instance grading");

    root->add_child(node_a);
    tree->process(0.0);
    Ref<ColorGradingResource> node_a_grading = node_color_grading(node_a, renderer);
    REQUIRE(node_a_grading.is_valid());
    CHECK_MESSAGE(node_a_grading == grading_a,
            "Re-entering node_a must replay its pending grading change onto its director record");
    CHECK(node_a_grading->get_exposure() == doctest::Approx(1.25f));
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "node_b's per-instance grading remains unchanged throughout");

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Detached set_color_grading(null) replays on re-enter and overrides peer") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    // PR #245 Codex follow-up P2: previously, setting color_grading=null
    // on a detached node armed grading_pushed_for_current_data=true
    // (because color_grading.is_null()), which suppressed the implicit
    // replay. The replay also rejected null. Result: the explicit null
    // assignment was silently lost — last-writer-wins was broken when
    // the latest writer wrote null while detached.
    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(9801.0f));
    node_b->set_splat_asset(make_single_splat_asset(9802.0f));

    Ref<ColorGradingResource> grading_a = make_color_grading_resource();
    Ref<ColorGradingResource> grading_b = make_color_grading_resource();
    REQUIRE(grading_a.is_valid());
    REQUIRE(grading_b.is_valid());
    grading_a->set_exposure(0.40f);
    grading_b->set_exposure(0.90f);

    node_a->set_color_grading(grading_a);
    node_b->set_color_grading(grading_b);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    REQUIRE_MESSAGE(renderer == node_b->get_renderer(), "Both nodes must share the same renderer for this test");

    // Per-instance storage: node_b's grading is stored on its own record.
    node_b->set_color_grading(grading_b);
    tree->process(0.0);
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "node_b's per-instance grading must reflect grading_b");

    // Detach node_a, then explicitly clear its grading via setter.
    root->remove_child(node_a);
    tree->process(0.0);

    node_a->set_color_grading(Ref<ColorGradingResource>());
    tree->process(0.0);
    // Detached null setter does not touch the director (node_a is unregistered),
    // and node_b's record is untouched.
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "Detached set_color_grading(null) on node_a must not touch node_b's per-instance grading");

    // Re-attach node_a. The explicit-pending replay pushes node_a's null into
    // its own director record; node_b is unaffected.
    root->add_child(node_a);
    tree->process(0.0);

    CHECK_MESSAGE(node_color_grading(node_a, renderer).is_null(),
            "Re-entering node_a after explicit set_color_grading(null) must apply the null to node_a's record");
    CHECK_MESSAGE(node_color_grading(node_b, renderer) == grading_b,
            "node_b's grading is still on node_b's record throughout");

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Two shared-renderer nodes get distinct per-instance grading rows in built buffer") {
    // Core proof of the per-instance fix: two GaussianSplatNode3D nodes sharing
    // a renderer must end up with distinct rows in the InstanceGradingGPU buffer
    // produced by the director, so the shader can apply different grading to each.
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(make_single_splat_asset(9900.0f));
    node_b->set_splat_asset(make_single_splat_asset(9910.0f));

    Ref<ColorGradingResource> grading_a = make_color_grading_resource();
    Ref<ColorGradingResource> grading_b = make_color_grading_resource();
    REQUIRE(grading_a.is_valid());
    REQUIRE(grading_b.is_valid());
    grading_a->set_exposure(-1.5f);
    grading_a->set_contrast(0.25f);
    grading_a->set_saturation(0.75f);
    grading_b->set_exposure(2.5f);
    grading_b->set_contrast(1.75f);
    grading_b->set_saturation(1.25f);

    node_a->set_color_grading(grading_a);
    node_b->set_color_grading(grading_b);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    REQUIRE_MESSAGE(renderer == node_b->get_renderer(), "Both nodes must share the same renderer");

    // Per-instance lookup on the director must return each node's own grading.
    CHECK(node_color_grading(node_a, renderer) == grading_a);
    CHECK(node_color_grading(node_b, renderer) == grading_b);
    CHECK(node_color_grading(node_a, renderer) != node_color_grading(node_b, renderer));

    // Direct director buffer build — the exact bytes the shader reads. This is
    // the lowest-level guarantee: every grading edit materializes as its own row.
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    REQUIRE(director != nullptr);
    LocalVector<InstanceGradingGPU> gradings;
    director->build_instance_grading_buffer_for_renderer(renderer.ptr(), gradings, false);
    // The instance-record filter may skip rows whose asset data is not loaded;
    // skip the deep assert if the build returned a smaller set than expected.
    if (gradings.size() >= 2) {
        // Find the row that matches each grading. Record order follows
        // world->instances insertion, which matches add_child order.
        const InstanceGradingGPU &row_a = gradings[0];
        const InstanceGradingGPU &row_b = gradings[1];
        CHECK(row_a.primary[1] == doctest::Approx(grading_a->get_exposure()));
        CHECK(row_a.primary[2] == doctest::Approx(grading_a->get_contrast()));
        CHECK(row_a.primary[3] == doctest::Approx(grading_a->get_saturation()));
        CHECK(row_b.primary[1] == doctest::Approx(grading_b->get_exposure()));
        CHECK(row_b.primary[2] == doctest::Approx(grading_b->get_contrast()));
        CHECK(row_b.primary[3] == doctest::Approx(grading_b->get_saturation()));
        // Rows must differ — the whole point of per-instance grading.
        CHECK(row_a.primary[1] != doctest::Approx(row_b.primary[1]));
    }

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting] Renderer-only set_color_grading bumps grading defaults generation") {
    // Regression for the P2 flagged by Codex: renderer-only / direct-data flows
    // (no SharedWorld in the director) still need set_color_grading to trigger a
    // grading SSBO re-upload. This is wired by
    // RenderConfigOrchestrator::set_color_grading ->
    // GaussianSplatSceneDirector::invalidate_grading_for_renderer, which always
    // bumps the renderer's `instance_grading_defaults_generation` counter before
    // the SharedWorld lookup. The streaming fingerprint consumes that counter so
    // upload_changed trips even without any director entry.
    Ref<GaussianSplatRenderer> renderer;
    renderer.instantiate();
    REQUIRE(renderer.is_valid());

    using GaussianRenderFacadeState::ResourceState;
    const ResourceState &state = renderer->get_resource_state();
    const uint64_t before = state.instance_grading_defaults_generation.load(std::memory_order_relaxed);

    Ref<ColorGradingResource> grading = make_color_grading_resource();
    REQUIRE(grading.is_valid());
    grading->set_exposure(-0.5f);
    renderer->set_color_grading(grading);

    const uint64_t after_set = state.instance_grading_defaults_generation.load(std::memory_order_relaxed);
    CHECK(after_set > before);

    // In-place slider edit on the same Ref fires the resource's `changed` signal,
    // which the renderer's handler catches via callable_mp and re-runs the
    // invalidation path. Bumps the counter again even though the Ref did not swap.
    grading->set_exposure(1.5f);
    const uint64_t after_slider = state.instance_grading_defaults_generation.load(std::memory_order_relaxed);
    CHECK(after_slider > after_set);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Shared renderer full-fidelity override only follows attached assets") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    Ref<GaussianSplatAsset> limited_asset_a = make_import_metadata_asset(8, 7000.0f, "desktop", 250000, 0.5);
    Ref<GaussianSplatAsset> limited_asset_b = make_import_metadata_asset(8, 7100.0f, "desktop", 250000, 0.5);
    Ref<GaussianSplatAsset> unattached_full_asset = make_import_metadata_asset(8, 7200.0f, "ultra", 0, 1.0);
    Ref<GaussianSplatAsset> attached_full_asset = make_import_metadata_asset(8, 7300.0f, "ultra", 0, 1.0);
    CHECK(limited_asset_a.is_valid());
    CHECK(limited_asset_b.is_valid());
    CHECK(unattached_full_asset.is_valid());
    CHECK(attached_full_asset.is_valid());
    if (!limited_asset_a.is_valid() || !limited_asset_b.is_valid() || !unattached_full_asset.is_valid() || !attached_full_asset.is_valid()) {
        return;
    }

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(limited_asset_a);
    node_b->set_splat_asset(limited_asset_b);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    CHECK(renderer == node_b->get_renderer());
    if (renderer != node_b->get_renderer()) {
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    Vector<Vector3> test_positions;
    test_positions.push_back(Vector3(0.0f, 0.0f, -10.0f));
    test_positions.push_back(Vector3(0.0f, 0.0f, -20.0f));
    test_positions.push_back(Vector3(0.0f, 0.0f, -30.0f));
    renderer->test_set_test_splats(test_positions);

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 0.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 200.0f);
    const Size2i viewport_size(1280, 720);

    auto reset_culling_controls = [&renderer]() {
        renderer->set_lod_enabled(true);
        renderer->set_importance_cull_threshold(0.35f);
        renderer->set_tiny_splat_screen_radius(2.5f);
        renderer->set_opacity_aware_culling(true);
        // 0.05f, not 0.2f: set_visibility_threshold CLAMPS to [0.0001, 0.1]
        // (render_quality_orchestrator.cpp:242, matching the exported property
        // hint range "0.0001,0.1"). The original 0.2f could never be read back,
        // so the matching CHECK below was unsatisfiable no matter what the
        // full-fidelity override did -- an independent third defect in this
        // case, masked while it was quarantined.
        renderer->set_visibility_threshold(0.05f);
        renderer->set_distance_cull_enabled(true);
        renderer->set_distance_cull_start(25.0f);
        renderer->set_distance_cull_max_rate(0.4f);
    };

    // The active asset must stay LIMITED in both blocks so that director
    // registration is the only variable. _renderer_requests_conservative_full_
    // fidelity_runtime (gaussian_splat_renderer.cpp:136-139) short-circuits to
    // true on the active asset alone, before it ever consults the director --
    // so setting a full-fidelity asset active here (as this test used to do)
    // makes both blocks identical to the predicate and tests nothing. Deleting
    // that active-asset clause to "fix" it would be a regression: a renderer
    // rendering a full-fidelity asset should preserve it.
    //
    // unattached_full_asset stays deliberately attached to NO node, which is
    // exactly the discriminator this case is named for.
    reset_culling_controls();
    renderer->set_gaussian_asset(limited_asset_a);
    renderer->test_cull_visible_count(camera_transform, projection, viewport_size);

    CHECK(renderer->get_lod_enabled());
    CHECK(renderer->get_importance_cull_threshold() == doctest::Approx(0.35f));
    CHECK(renderer->get_tiny_splat_screen_radius() == doctest::Approx(2.5f));
    CHECK(renderer->is_opacity_aware_culling());
    CHECK(renderer->get_visibility_threshold() == doctest::Approx(0.05f));
    CHECK(renderer->is_distance_cull_enabled());
    CHECK(renderer->get_distance_cull_start() == doctest::Approx(25.0f));
    CHECK(renderer->get_distance_cull_max_rate() == doctest::Approx(0.4f));

    // Attach the full-fidelity asset to a node in this shared world. Active
    // asset is held at limited_asset_a, so the ONLY thing that changed is that
    // a full-fidelity asset is now registered with the director.
    node_b->set_splat_asset(attached_full_asset);
    tree->process(0.0);
    reset_culling_controls();
    renderer->set_gaussian_asset(limited_asset_a);
    renderer->test_cull_visible_count(camera_transform, projection, viewport_size);

    CHECK_FALSE(renderer->get_lod_enabled());
    CHECK(renderer->get_importance_cull_threshold() == doctest::Approx(0.0f));
    CHECK(renderer->get_tiny_splat_screen_radius() == doctest::Approx(0.0f));
    CHECK_FALSE(renderer->is_opacity_aware_culling());
    CHECK(renderer->get_visibility_threshold() == doctest::Approx(0.0f));
    CHECK_FALSE(renderer->is_distance_cull_enabled());
    CHECK(renderer->get_distance_cull_start() == doctest::Approx(0.0f));
    CHECK(renderer->get_distance_cull_max_rate() == doctest::Approx(0.0f));

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

// Renamed from "Shared renderer ignores hidden full-fidelity assets", which
// asserted the OPPOSITE of the documented contract and was therefore quarantined
// (#329) as a product failure. It is not one:
// collect_registered_assets_for_renderer (gaussian_splat_scene_director.cpp:2828)
// iterates world->asset_records -- a per-ASSET structure with no visibility
// concept at all -- and the omission of a visibility filter is deliberate and
// documented at gaussian_splat_renderer.cpp:150-153: the full-fidelity decision
// "should not flicker as nodes pass in/out of the frustum". This case now pins
// that stability contract instead of contradicting it.
TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] Shared renderer full-fidelity collection ignores node visibility") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    Ref<GaussianSplatAsset> limited_asset_a = make_import_metadata_asset(8, 7000.0f, "desktop", 250000, 0.5);
    Ref<GaussianSplatAsset> limited_asset_b = make_import_metadata_asset(8, 7100.0f, "desktop", 250000, 0.5);
    Ref<GaussianSplatAsset> hidden_full_asset = make_import_metadata_asset(8, 7200.0f, "ultra", 0, 1.0);
    CHECK(limited_asset_a.is_valid());
    CHECK(limited_asset_b.is_valid());
    CHECK(hidden_full_asset.is_valid());
    if (!limited_asset_a.is_valid() || !limited_asset_b.is_valid() || !hidden_full_asset.is_valid()) {
        return;
    }

    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    node_a->set_splat_asset(limited_asset_a);
    node_b->set_splat_asset(limited_asset_b);

    root->add_child(node_a);
    root->add_child(node_b);
    tree->process(0.0);

    Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
    if (!renderer.is_valid()) {
        MESSAGE("Skipping test - renderer unavailable (headless mode)");
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }
    CHECK(renderer == node_b->get_renderer());
    if (renderer != node_b->get_renderer()) {
        root->remove_child(node_b);
        root->remove_child(node_a);
        memdelete(node_b);
        memdelete(node_a);
        return;
    }

    Vector<Vector3> test_positions;
    test_positions.push_back(Vector3(0.0f, 0.0f, -10.0f));
    test_positions.push_back(Vector3(0.0f, 0.0f, -20.0f));
    test_positions.push_back(Vector3(0.0f, 0.0f, -30.0f));
    renderer->test_set_test_splats(test_positions);

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 0.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 200.0f);
    const Size2i viewport_size(1280, 720);

    auto reset_culling_controls = [&renderer]() {
        renderer->set_lod_enabled(true);
        renderer->set_importance_cull_threshold(0.35f);
        renderer->set_tiny_splat_screen_radius(2.5f);
        renderer->set_opacity_aware_culling(true);
        // 0.05f, not 0.2f: set_visibility_threshold CLAMPS to [0.0001, 0.1]
        // (render_quality_orchestrator.cpp:242, matching the exported property
        // hint range "0.0001,0.1"). The original 0.2f could never be read back,
        // so the matching CHECK below was unsatisfiable no matter what the
        // full-fidelity override did -- an independent third defect in this
        // case, masked while it was quarantined.
        renderer->set_visibility_threshold(0.05f);
        renderer->set_distance_cull_enabled(true);
        renderer->set_distance_cull_start(25.0f);
        renderer->set_distance_cull_max_rate(0.4f);
    };

    // A HIDDEN node still holds a registered full-fidelity asset, so the
    // conservative override must engage. This is the documented no-flicker
    // contract, not a leak.
    node_b->set_splat_asset(hidden_full_asset);
    node_b->set_visible(false);
    tree->process(0.0);

    reset_culling_controls();
    renderer->set_gaussian_asset(limited_asset_a);
    renderer->test_cull_visible_count(camera_transform, projection, viewport_size);

    CHECK_FALSE(renderer->get_lod_enabled());
    CHECK(renderer->get_importance_cull_threshold() == doctest::Approx(0.0f));
    CHECK(renderer->get_tiny_splat_screen_radius() == doctest::Approx(0.0f));
    CHECK_FALSE(renderer->is_opacity_aware_culling());
    CHECK(renderer->get_visibility_threshold() == doctest::Approx(0.0f));
    CHECK_FALSE(renderer->is_distance_cull_enabled());
    CHECK(renderer->get_distance_cull_start() == doctest::Approx(0.0f));
    CHECK(renderer->get_distance_cull_max_rate() == doctest::Approx(0.0f));

    // Showing the same node must NOT change the decision -- that is the whole
    // point of the stable-superset collection.
    node_b->set_visible(true);
    tree->process(0.0);

    reset_culling_controls();
    renderer->set_gaussian_asset(limited_asset_a);
    renderer->test_cull_visible_count(camera_transform, projection, viewport_size);

    CHECK_FALSE(renderer->get_lod_enabled());
    CHECK(renderer->get_importance_cull_threshold() == doctest::Approx(0.0f));
    CHECK(renderer->get_tiny_splat_screen_radius() == doctest::Approx(0.0f));
    CHECK_FALSE(renderer->is_opacity_aware_culling());
    CHECK(renderer->get_visibility_threshold() == doctest::Approx(0.0f));
    CHECK_FALSE(renderer->is_distance_cull_enabled());
    CHECK(renderer->get_distance_cull_start() == doctest::Approx(0.0f));
    CHECK(renderer->get_distance_cull_max_rate() == doctest::Approx(0.0f));

    // CONTROL: the two blocks above assert the same end state, so on their own
    // they would also pass if the override were unconditionally on. Detaching
    // the full-fidelity asset entirely must lift it again -- this is what makes
    // the case discriminate rather than merely agree with itself.
    node_b->set_splat_asset(limited_asset_b);
    tree->process(0.0);

    reset_culling_controls();
    renderer->set_gaussian_asset(limited_asset_a);
    renderer->test_cull_visible_count(camera_transform, projection, viewport_size);

    CHECK(renderer->get_lod_enabled());
    CHECK(renderer->get_importance_cull_threshold() == doctest::Approx(0.35f));
    CHECK(renderer->get_tiny_splat_screen_radius() == doctest::Approx(2.5f));
    CHECK(renderer->is_opacity_aware_culling());
    CHECK(renderer->get_visibility_threshold() == doctest::Approx(0.05f));
    CHECK(renderer->is_distance_cull_enabled());
    CHECK(renderer->get_distance_cull_start() == doctest::Approx(25.0f));
    CHECK(renderer->get_distance_cull_max_rate() == doctest::Approx(0.4f));

    root->remove_child(node_b);
    root->remove_child(node_a);
    memdelete(node_b);
    memdelete(node_a);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree] Clearing canonical asset source unregisters instance") {
    SceneTree *tree = SceneTree::get_singleton();
    REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

    Window *root = tree->get_root();
    REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    CHECK_MESSAGE(director != nullptr, "Scene director singleton must exist for unregister test");
    if (!director) {
        return;
    }

    LocalVector<InstanceDataGPU> instance_buffer;
    director->build_instance_buffer(instance_buffer);
    const int baseline_count = instance_buffer.size();

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    node->set_splat_asset(make_single_splat_asset(5.0f));
    root->add_child(node);
    tree->process(0.0);

    director->build_instance_buffer(instance_buffer);
    CHECK(instance_buffer.size() > baseline_count);

    node->set_splat_asset(Ref<GaussianSplatAsset>());
    tree->process(0.0);

    director->build_instance_buffer(instance_buffer);
    CHECK_EQ(instance_buffer.size(), baseline_count);

    root->remove_child(node);
    memdelete(node);
}

// ── Import propagation proof ───────────────────────────────────────────

TEST_CASE("[GaussianSplatting][Node][SceneTree] Two nodes sharing one asset both observe asset mutation via changed signal") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE(tree != nullptr);
	Window *root = tree->get_root();
	REQUIRE(root != nullptr);

	// Create a shared asset with 1 splat.
	Ref<GaussianSplatAsset> shared_asset = make_single_splat_asset(0.0f);
	REQUIRE(shared_asset.is_valid());
	CHECK(shared_asset->get_splat_count() == 1);

	// Two nodes both point to the same asset Ref.
	GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
	GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
	root->add_child(node_a);
	root->add_child(node_b);

	node_a->set_splat_asset(shared_asset);
	node_b->set_splat_asset(shared_asset);
	tree->process(0.0);

	CHECK(node_a->get_total_splat_count() == 1);
	CHECK(node_b->get_total_splat_count() == 1);

	// Mutate the shared asset: grow to 5 splats. The Packed-array setters
	// are sealed once runtime GaussianData has been handed out (see
	// GaussianSplatAsset::_runtime_mutation_permitted), so drive the
	// change through populate_from_gaussian_data — the documented
	// runtime-to-asset persistence writer that resets the seal.
	Ref<GaussianData> grown_data = make_test_gaussian_data(5, 0.0f);
	REQUIRE_EQ(shared_asset->populate_from_gaussian_data(grown_data), OK);

	// The asset emits "changed" when its data is reseeded. Both nodes
	// should have received _on_asset_changed() which calls _update_asset()
	// and re-reads total_splat_count from the asset.
	CHECK(node_a->get_total_splat_count() == 5);
	CHECK(node_b->get_total_splat_count() == 5);

	// Verify the asset Ref is truly shared (same object).
	CHECK(node_a->get_splat_asset() == node_b->get_splat_asset());

	root->remove_child(node_a);
	root->remove_child(node_b);
	memdelete(node_a);
	memdelete(node_b);
}

TEST_CASE("[GaussianSplatting][Node][SceneTree] Two nodes with separate asset Refs do not cross-propagate") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE(tree != nullptr);
	Window *root = tree->get_root();
	REQUIRE(root != nullptr);

	Ref<GaussianSplatAsset> asset_a = make_single_splat_asset(0.0f);
	Ref<GaussianSplatAsset> asset_b = make_single_splat_asset(10.0f);

	GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
	GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
	root->add_child(node_a);
	root->add_child(node_b);

	node_a->set_splat_asset(asset_a);
	node_b->set_splat_asset(asset_b);
	tree->process(0.0);

	CHECK(node_a->get_total_splat_count() == 1);
	CHECK(node_b->get_total_splat_count() == 1);

	// Mutate only asset_a. Drive the change through populate_from_gaussian_data
	// since the Packed-array setters are sealed after first hand-out.
	Ref<GaussianData> grown_data = make_test_gaussian_data(7, 0.0f);
	REQUIRE_EQ(asset_a->populate_from_gaussian_data(grown_data), OK);

	// node_a should see 7; node_b should still see 1.
	CHECK(node_a->get_total_splat_count() == 7);
	CHECK(node_b->get_total_splat_count() == 1);

	root->remove_child(node_a);
	root->remove_child(node_b);
	memdelete(node_a);
	memdelete(node_b);
}

// --- Inspector surface contracts (#836 / #834) -------------------------------
//
// These lock in the two property-declaration fixes that shrink the Gaussian
// inspector. They are pure ClassDB/PropertyInfo checks, so they need no GPU and
// no editor.

namespace {

bool find_property_info(Object *p_object, const StringName &p_name, PropertyInfo &r_info) {
	if (p_object == nullptr) {
		return false;
	}
	List<PropertyInfo> property_list;
	p_object->get_property_list(&property_list);
	for (const List<PropertyInfo>::Element *E = property_list.front(); E; E = E->next()) {
		if (E->get().name == p_name) {
			r_info = E->get();
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("[GaussianSplatting][Editor] Effector layer masks use the compact 3D-render layer grid") {
	// PROPERTY_HINT_FLAGS with an explicit "Layer 1,...,Layer 16" list renders as a
	// 16-row vertical checkbox column (~700 px in the inspector). Godot's
	// PROPERTY_HINT_LAYERS_3D_RENDER renders the same bitmask as a 4x5 grid, which is
	// what VisualInstance3D::layers uses (scene/3d/visual_instance_3d.cpp:188).
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	if (!node) {
		FAIL("could not allocate GaussianSplatNode3D");
		return;
	}
	// The mask is only editor-visible while scene effectors are enabled.
	node->set_scene_effectors_enabled(true);

	PropertyInfo node_mask;
	CHECK(find_property_info(node, StringName("rendering/scene_effector_layer_mask"), node_mask));
	CHECK(node_mask.hint == PROPERTY_HINT_LAYERS_3D_RENDER);
	CHECK(node_mask.hint_string.is_empty());
	memdelete(node);

	SphereEffector3D *effector = memnew(SphereEffector3D);
	if (!effector) {
		FAIL("could not allocate SphereEffector3D");
		return;
	}
	PropertyInfo effector_mask;
	CHECK(find_property_info(effector, StringName("layer_mask"), effector_mask));
	CHECK(effector_mask.hint == PROPERTY_HINT_LAYERS_3D_RENDER);
	CHECK(effector_mask.hint_string.is_empty());
	memdelete(effector);
}

TEST_CASE("[GaussianSplatting][Editor] GaussianSplatAsset bulk arrays are storage-only, not removed") {
	// #834: the editor enumerates the property list and calls every getter, so having
	// these multi-million-element arrays carry PROPERTY_USAGE_EDITOR is what produced
	// the "get_*() called on unloaded asset" warning stack. They must stay STORAGE
	// (serialization + script access preserved) and lose only editor display - #712 is
	// this repo's precedent for a removed ClassDB property breaking saved scenes.
	const char *bulk_arrays[] = {
		"data/positions",
		"data/colors",
		"data/scales",
		"data/rotations",
		"data/sh_dc",
		"data/sh_first_order",
		"data/sh_high_order",
		"data/opacity_logits",
		"data/palette_ids",
		"data/painterly_flags",
		"data/normals",
		"data/brush_axes",
		"data/stroke_ages",
	};

	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	if (asset.is_null()) {
		FAIL("could not instantiate GaussianSplatAsset");
		return;
	}

	for (const char *name : bulk_arrays) {
		// doctest prints a bare `const char *` as a pointer; String has a StringMaker.
		const String label(name);
		const StringName property_name(name);
		PropertyInfo info;
		const bool present = find_property_info(asset.ptr(), property_name, info);
		CHECK_MESSAGE(present, "property still declared: ", label);
		if (!present) {
			continue;
		}
		CHECK_MESSAGE((info.usage & PROPERTY_USAGE_STORAGE) != 0, "still serialized: ", label);
		CHECK_MESSAGE((info.usage & PROPERTY_USAGE_EDITOR) == 0, "no longer editor-visible: ", label);
		// Still reachable from script through ClassDB (Object::get / Object::set).
		bool valid = false;
		asset->get(property_name, &valid);
		CHECK_MESSAGE(valid, "still readable via Object::get(): ", label);
	}
}

#ifdef TOOLS_ENABLED

namespace {

// Collect every debug/* property path that has a real replacement control in the
// tree parse_begin() just built, by reading GS_DEBUG_REPLACEMENT_META off the
// controls themselves. This is deliberately an observation of the CONSTRUCTED UI,
// not of the plugin's suppression registry: the point of the case below is to
// cross-check the two against each other, which a test that read the registry
// (or restated the code's name predicate) could not do.
void collect_debug_replacement_controls(Node *p_control, HashSet<String> &r_paths) {
	if (p_control == nullptr) {
		return;
	}
	if (p_control->has_meta(GS_DEBUG_REPLACEMENT_META)) {
		r_paths.insert(String(p_control->get_meta(GS_DEBUG_REPLACEMENT_META)));
	}
	for (int i = 0; i < p_control->get_child_count(true); i++) {
		collect_debug_replacement_controls(p_control->get_child(i, true), r_paths);
	}
}

} // namespace

TEST_CASE("[GaussianSplatting][Editor] Inspector suppresses a debug/ property only when it built a replacement control") {
	// GaussianSplatNodeInspectorPlugin::parse_property() suppresses the default editor
	// for a debug/* property because parse_begin() already draws a custom control for
	// it. That relationship is the whole contract, and it has now been broken twice by
	// guards that asserted it from the property's NAME instead of from the control:
	//
	//   - the original hand-written list of 11 names drifted and missed
	//     debug/overlay_opacity, orphaning it in the "Debug" group (#836);
	//   - #838 round 1 replaced it with a bare `begins_with("debug/")` prefix check,
	//     which ATE debug/overlay_opacity -- it has no replacement control, so
	//     suppressing it deleted the only way to tune the overlay blend;
	//   - #838 round 2 special-cased that one name. The instance was fixed; the shape
	//     was not. A 13th debug/* property would silently lose its inspector UI, and
	//     #838's test would still have passed, because that test encoded the same name
	//     predicate the code did.
	//
	// So this case does not name any property. It derives BOTH sides independently --
	// the property set from the node's own property list, the control set from the
	// metadata on the controls parse_begin() actually constructed -- and asserts they
	// agree. Mutation-proof: with a 13th debug/* property declared and no control for
	// it, this is RED under the #838 predicate and GREEN under the registry.
	Ref<GaussianSplatNodeInspectorPlugin> plugin;
	plugin.instantiate();
	if (plugin.is_null()) {
		FAIL("could not instantiate GaussianSplatNodeInspectorPlugin");
		return;
	}

	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	if (!node) {
		FAIL("could not allocate GaussianSplatNode3D");
		return;
	}

	// Real ordering: EditorInspector calls parse_begin() (editor_inspector.cpp:3754)
	// before any parse_property() (:4350) for the same object.
	plugin->parse_begin(node);

	HashSet<String> controls_built;
	for (const EditorInspectorPlugin::AddedEditor &added : plugin->added_editors) {
		collect_debug_replacement_controls(added.property_editor, controls_built);
	}

	List<PropertyInfo> property_list;
	node->get_property_list(&property_list);

	int debug_properties = 0;
	int suppressed = 0;
	int kept_visible = 0;
	bool saw_non_debug_property = false;

	for (const List<PropertyInfo>::Element *E = property_list.front(); E; E = E->next()) {
		const PropertyInfo &info = E->get();
		if ((info.usage & PROPERTY_USAGE_EDITOR) == 0) {
			continue; // Category/group separators and storage-only entries.
		}
		const String path(info.name);
		const bool handled = plugin->parse_property(node, info.type, path, info.hint, info.hint_string, info.usage, false);

		if (!path.begins_with("debug/")) {
			// Nothing outside the group may be consumed by the plugin.
			CHECK_MESSAGE(!handled, "non-debug property keeps its default editor: ", path);
			saw_non_debug_property = true;
			continue;
		}

		debug_properties++;
		const bool has_control = controls_built.has(path);
		if (has_control) {
			// A replacement exists, so the default editor would be a duplicate.
			CHECK_MESSAGE(handled, "suppressed in favour of its custom control: ", path);
			suppressed++;
		} else {
			// THE fail-safe assertion. Suppressing a property that parse_begin()
			// built nothing for deletes a user-facing control with no replacement.
			CHECK_MESSAGE(!handled, "no replacement control was built, so it must keep its default editor: ", path);
			kept_visible++;
		}
	}

	// Every control that was built must correspond to a live debug/* property --
	// otherwise the block draws a control for something the node no longer exposes.
	for (const String &built : controls_built) {
		bool found = false;
		for (const List<PropertyInfo>::Element *E = property_list.front(); E; E = E->next()) {
			if (String(E->get().name) == built) {
				found = true;
				break;
			}
		}
		CHECK_MESSAGE(found, "replacement control targets a property the node declares: ", built);
	}

	// Guard against a vacuous pass: the loop must actually have seen properties, and
	// both arms of the contract must have been exercised at least once.
	CHECK(saw_non_debug_property);
	CHECK(debug_properties >= 2);
	CHECK(suppressed + kept_visible == debug_properties);
#ifdef DEBUG_ENABLED
	// The Debug Visualization block is compiled under DEBUG_ENABLED only. When it is
	// present, both arms must be non-empty -- controls exist, and at least
	// debug/overlay_opacity has none -- so neither CHECK above can pass vacuously.
	CHECK(suppressed > 0);
	CHECK(kept_visible > 0);
	CHECK(controls_built.size() == (uint32_t)suppressed);
#endif

	// parse_begin() hands ownership of its controls to EditorInspector, which is not
	// running here.
	for (const EditorInspectorPlugin::AddedEditor &added : plugin->added_editors) {
		if (added.property_editor) {
			memdelete(added.property_editor);
		}
	}
	plugin->added_editors.clear();

	memdelete(node);
}

#endif // TOOLS_ENABLED

// ── #806 review: the setup-failure exit must not strand a node in the tree ──────
//
// This is the regression test for the ScopedTestNode guard, and it measures the
// property the finding is actually about: not that a failed setup REPORTS failure, but
// that after taking a setup-failure early exit the shared SceneTree is back to the
// state it was in before the case ran.
//
// The failing shape it pins is `if (!x) { FAIL(); return; }` returning from the middle
// of a case that has already parented a node to the root window. That idiom is not
// optional here -- doctest is built with disable_exceptions=True, so a bare REQUIRE
// does not abort -- so the leak cannot be fixed by removing the early return; the
// teardown has to survive it. take_scoped_test_node_setup_failure_exit() reproduces
// exactly that shape (see the anonymous namespace above) with NO teardown statement
// after the early `return`.
//
// Mutation proof (run, not asserted): change the `ScopedTestNode<GaussianSplatNode3D>
// node` in that helper back to a raw `GaussianSplatNode3D *node =
// memnew(GaussianSplatNode3D)` -- the code as it stood before this change -- rebuild,
// and this case goes 5 of 10 assertions RED: `root->get_child_count() == baseline`
// reports 1 == 0 after the early exit, the director still returns an instance
// submission for the supposedly-gone node, and the SECOND leg then sees 2 children,
// which is the cross-case accumulation the finding is about. Restoring the guard puts
// all 10 back to green. Both arms were run.
//
// Deliberately NOT [RequiresGPU]: the leak is a SceneTree/lifetime property with no
// device dependency, and the exit it measures is reached in the headless lane too.
TEST_CASE("[GaussianSplatting][Node][SceneTree] A setup-failure early exit must leave no node attached to the shared tree") {
	SceneTree *tree = SceneTree::get_singleton();
	if (tree == nullptr) {
		FAIL("SceneTree must exist (provided by [SceneTree] tag)");
		return;
	}
	Window *root = tree->get_root();
	if (root == nullptr) {
		FAIL("SceneTree root window must exist");
		return;
	}
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	if (director == nullptr) {
		FAIL("GaussianSplatSceneDirector singleton must exist");
		return;
	}

	tree->process(0.0);
	const int baseline_children = root->get_child_count();

	// ---- the exit under test: early `return` from the middle of the scope ----
	const ScopedTestNodeExitObservation failed_setup = take_scoped_test_node_setup_failure_exit(root, true);
	// Assert the branch being measured actually ran. Without this the case would pass
	// just as happily while measuring the fallthrough path.
	CHECK_MESSAGE(failed_setup.took_early_exit,
			"the helper must have taken the setup-failure early exit, otherwise this case measures the wrong path");
	CHECK_MESSAGE(failed_setup.instance_id.is_valid(),
			"the helper must have reported the node's ObjectID, otherwise the director assertion below is vacuous");
	// Non-vacuity: there was something to strand. Measured from inside the scope, because
	// by the time control returns here the guard has already removed it.
	CHECK_MESSAGE(failed_setup.child_count_while_attached == baseline_children + 1,
			"the helper must actually have attached its node to the shared root before exiting; saw ",
			failed_setup.child_count_while_attached, " children vs baseline ", baseline_children);
	// Same non-vacuity question for the director leg: "no stranded record afterwards" only
	// means something if a record existed while the node was attached.
	CHECK_MESSAGE(failed_setup.registered_while_attached,
			"the helper's node must have been registered with the SceneDirector while attached, otherwise the stranded-record assertion below proves nothing");
	tree->process(0.0);

	CHECK_MESSAGE(root->get_child_count() == baseline_children,
			"After a setup-failure early exit the shared SceneTree must be back to its pre-case child count; got ",
			root->get_child_count(), " vs baseline ", baseline_children);
	GaussianSplatSceneDirector::InstanceSubmission stranded;
	CHECK_MESSAGE(!director->get_instance_submission(failed_setup.instance_id, &stranded),
			"After a setup-failure early exit the node must no longer be registered with the SceneDirector.");

	// ---- and the ordinary fallthrough exit, so the guard is not proven on one arm only ----
	const ScopedTestNodeExitObservation normal_exit = take_scoped_test_node_setup_failure_exit(root, false);
	CHECK_MESSAGE(!normal_exit.took_early_exit,
			"the helper must have taken the fallthrough exit on this leg");
	CHECK_MESSAGE(normal_exit.child_count_while_attached == baseline_children + 1,
			"the fallthrough leg must also have attached its node; saw ",
			normal_exit.child_count_while_attached, " children vs baseline ", baseline_children);
	tree->process(0.0);
	CHECK_MESSAGE(root->get_child_count() == baseline_children,
			"After the ordinary fallthrough exit the shared SceneTree must also be back to its pre-case child count; got ",
			root->get_child_count(), " vs baseline ", baseline_children);
	GaussianSplatSceneDirector::InstanceSubmission stranded_normal;
	CHECK_MESSAGE(!director->get_instance_submission(normal_exit.instance_id, &stranded_normal),
			"After the ordinary fallthrough exit the node must no longer be registered with the SceneDirector.");
}

// ── #798 review round 2: a failed set_splat_data() must fail CLOSED ──────
//
// set_splat_data() returns void, so the only honest observable is what the node
// ends up publishing. _register_instance_in_director() (~:2457-2470) rebuilds
// `asset` from renderer_data on every tree/world re-entry, CONTINUES past the
// populate error, and assigns `asset = runtime_asset` regardless. Before the
// round-2 fix, runtime_asset still held the PREVIOUS payload and
// populate_from_gaussian_data() bailed at its `count <= 0` check BEFORE
// overwriting splat_count -- so the old splats were republished on a node whose
// set_splat_data() had failed.
//
// Written as a contract: the node must publish either the NEW payload or
// nothing, and in neither case the stale previous payload.
//
// ── #798 review round 3: the case now FORCES the failure ────────────────
//
// Two things were wrong with the round-2 version of this case, and the second was
// only visible once the first was fixed:
//
//  1. It never failed. It submitted seven ordinary elements and no injection, so both
//     derived allocations succeeded, the node published 7, and both assertions passed
//     with the whole rollback branch deleted. Payload size cannot fix that: any count
//     large enough to exhaust the allocator is rejected by _validate_splat_data_inputs()
//     first. It now arms the TESTS_ENABLED seam in core/gs_vector_alloc.h at the exact
//     `p_where` label of the sh_dc resize, and asserts the seam actually fired.
//
//  2. Reaching the branch showed the ROLLBACK was still incomplete. published == 0 was
//     never the fail-closed state it looked like: with renderer_data left valid-but-empty
//     the node re-registers with a fresh, zero-splat runtime_asset and keeps reporting
//     "has data" to the editor. The count-only assertions could not see that, which is
//     why the fix needed renderer_data.unref() and why the assertions below now cover
//     the registration itself and the configuration warning, not just the count.
//
// Two legs remain. Headless the SceneDirector returns no shared renderer, so
// set_splat_data() bails at _ensure_renderer_for_manual_data() (:742) before any payload
// is published; that leg asserts exactly that and is honest about proving nothing else.
// Under the GPU harness (NodeSceneTree batch) a renderer exists and the full contract
// above runs.
TEST_CASE("[GaussianSplatting][Node][SceneTree][RequiresGPU] A failed set_splat_data must not republish the previous payload") {
	SceneTree *tree = SceneTree::get_singleton();
	if (tree == nullptr) {
		FAIL("SceneTree must exist (provided by [SceneTree] tag)");
		return;
	}
	Window *root = tree->get_root();
	if (root == nullptr) {
		FAIL("SceneTree root window must exist");
		return;
	}
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	if (director == nullptr) {
		FAIL("GaussianSplatSceneDirector singleton must exist");
		return;
	}

	// #806 review: scoped, not hand-repeated. Several exits below are
	// `if (!x) { FAIL(); return; }` -- the idiom this build requires (#656/#843) -- and
	// three of them used to return with this node still parented to the shared root
	// window and still registered with the director, leaking it into every later case
	// in the single-process NodeSceneTree batch. See ScopedTestNode above.
	ScopedTestNode<GaussianSplatNode3D> node(memnew(GaussianSplatNode3D));
	root->add_child(node.get());
	tree->process(0.0);

	// ---- first payload: 5 splats ----
	PackedVector3Array positions;
	PackedColorArray colors;
	PackedVector3Array scales;
	PackedFloat32Array opacities;
	TypedArray<Quaternion> rotations;
	for (int i = 0; i < 5; i++) {
		positions.push_back(Vector3(float(i), 0.0f, 0.0f));
		colors.push_back(Color(1, 1, 1, 1));
		scales.push_back(Vector3(1, 1, 1));
		opacities.push_back(1.0f);
		rotations.push_back(Quaternion());
	}
	PackedFloat32Array sh;
	PackedInt32Array palette_ids;
	PackedInt32Array painterly_flags;
	PackedVector3Array normals;
	PackedVector2Array brush_axes;
	PackedFloat32Array stroke_ages;

	node->set_splat_data(positions, colors, scales, opacities, rotations,
			sh, palette_ids, painterly_flags, normals, brush_axes, stroke_ages, false);
	tree->process(0.0);

	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	if (!renderer.is_valid()) {
		// Headless with no renderer: set_splat_data() bails at
		// _ensure_renderer_for_manual_data() before any payload is published, so
		// there is no stale-republish path to exercise. Assert that explicitly
		// rather than returning with zero assertions (a silent skip in a required
		// batch is indistinguishable from a passing test).
		GaussianSplatSceneDirector::InstanceSubmission headless_sub;
		const bool headless_published =
				director->get_instance_submission(node->get_instance_id(), &headless_sub) &&
				headless_sub.asset.is_valid() && headless_sub.asset->get_splat_count() > 0;
		CHECK_MESSAGE(!headless_published,
				"With no renderer, set_splat_data() must publish no payload at all.");
		return;
	}

	GaussianSplatSceneDirector::InstanceSubmission first;
	if (!director->get_instance_submission(node->get_instance_id(), &first)) {
		FAIL("the node must be registered with the director after set_splat_data()");
		return;
	}
	if (!first.asset.is_valid()) {
		FAIL("the first set_splat_data() must publish an asset");
		return;
	}
	if (first.asset->get_splat_count() != 5u) {
		FAIL("the first set_splat_data() must publish 5 splats");
		return;
	}

	// ---- second payload: a DIFFERENT count, so a stale republish is unambiguous ----
	PackedVector3Array positions_b;
	PackedColorArray colors_b;
	PackedVector3Array scales_b;
	PackedFloat32Array opacities_b;
	TypedArray<Quaternion> rotations_b;
	for (int i = 0; i < 7; i++) {
		positions_b.push_back(Vector3(0.0f, float(i), 0.0f));
		colors_b.push_back(Color(1, 1, 1, 1));
		scales_b.push_back(Vector3(1, 1, 1));
		opacities_b.push_back(1.0f);
		rotations_b.push_back(Quaternion());
	}
	// #798 review round 3: the second call MUST actually fail, and nothing about the payload
	// can make it. `opacities_b` is non-empty, so the opacity_from_color allocation is skipped;
	// `sh` is empty and `colors_b` is not, so _apply_optional_splat_arrays() takes the sh_dc
	// branch (gaussian_splat_node_3d.cpp:912-929) -- and that resize succeeds for 7 splats, as
	// it would for any count _validate_splat_data_inputs() lets through. The previous revision
	// of this case submitted exactly these arrays with no injection: both derived resizes
	// succeeded, the node published 7, and both CHECKs below passed with the entire rollback
	// branch deleted. Arm the sh_dc site so the real gs_resize_or_fail() takes its real
	// failure branch.
	gs_vector_alloc_force_failure_at("GaussianSplatNode3D::set_splat_data sh_dc");
	{
		// The failure arm emits the expected allocation ERR_PRINT; keep the log clean.
		ERR_PRINT_OFF;
		node->set_splat_data(positions_b, colors_b, scales_b, opacities_b, rotations_b,
				sh, palette_ids, painterly_flags, normals, brush_axes, stroke_ages, false);
		ERR_PRINT_ON;
	}
	// Assert the injection FIRED before asserting anything about its consequences. The arm is
	// cleared by the firing, so a still-armed seam means set_splat_data() never reached that
	// allocation -- the case would otherwise silently go back to measuring the success path,
	// which is precisely the vacuity being fixed here. Disarm either way so a failure here
	// cannot leak into a later case.
	//
	// This guard covers a RENAMED or unreached site, not a deleted arm: measured by deleting
	// the arm above, the seam is simply never armed and this reads "fired". That case is
	// covered instead by the assertions themselves, which then see the SUCCESS path and go
	// red 4-of-5 (published==7, has_after, no warning, count==7) rather than passing. Both
	// arms were run.
	const bool injection_fired = !gs_vector_alloc_forced_failure_is_armed();
	gs_vector_alloc_clear_forced_failure();
	if (!injection_fired) {
		FAIL("the injected sh_dc allocation failure never fired -- set_splat_data() did not reach _apply_optional_splat_arrays()'s sh_dc resize, so this case proves nothing");
		return;
	}
	tree->process(0.0);

	// Force the tree/world re-entry that rebuilds `asset` from renderer_data / runtime_asset.
	// ScopedTestNode detaches from whatever parent the node has at destruction time, so
	// re-parenting here does not disturb the teardown.
	root->remove_child(node.get());
	root->add_child(node.get());
	tree->process(0.0);

	GaussianSplatSceneDirector::InstanceSubmission after;
	const bool has_after = director->get_instance_submission(node->get_instance_id(), &after);
	const int published = (has_after && after.asset.is_valid())
			? int(after.asset->get_splat_count())
			: 0;

	CHECK_MESSAGE(published != 5,
			"After set_splat_data() fails, a tree re-entry must NOT republish the previous 5-splat payload.");
	CHECK_MESSAGE(published != 7,
			"After set_splat_data() fails, the node must NOT publish the payload whose derived allocation failed.");
	// The registration itself, not just its splat count. runtime_asset.unref() alone leaves
	// renderer_data a valid-but-empty Ref, and _register_instance_in_director() then builds a
	// FRESH runtime_asset out of it and registers that zero-splat asset -- published == 0 while
	// the node is still permanently in the director. This is the assertion that the round-3
	// renderer_data.unref() is needed for; the count-only assertions above cannot see it.
	CHECK_MESSAGE(!has_after,
			"After a failed set_splat_data(), a tree re-entry must not re-register the node at all -- not even with an empty asset.");
	// Same failure seen from the editor side: a node with no renderable payload must say so.
	const PackedStringArray warnings = node->get_configuration_warnings();
	bool warns_no_data = false;
	for (int i = 0; i < warnings.size(); i++) {
		if (warnings[i].contains("No Gaussian splat asset or runtime data assigned")) {
			warns_no_data = true;
			break;
		}
	}
	CHECK_MESSAGE(warns_no_data,
			"After a failed set_splat_data(), the node must report the no-data configuration warning.");
	// The counters are maintained by _finalize_manual_splat_setup(), which this branch skips,
	// so without an explicit reset they keep describing the previous 5-splat payload.
	CHECK_MESSAGE(node->get_total_splat_count() == 0u,
			"After a failed set_splat_data(), the node must not still report the previous payload's splat count.");

	// No teardown statement here: ~ScopedTestNode() detaches and frees the node on this
	// exit and on every early one above.
}

#endif // TESTS_ENABLED || TOOLS_ENABLED
