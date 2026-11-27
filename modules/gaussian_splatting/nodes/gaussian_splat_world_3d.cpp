#include "gaussian_splat_world_3d.h"
#include "gaussian_splat_node_3d.h"

#include "../core/gaussian_splat_scene_director.h"
#include "../core/gs_project_settings.h"
#include "../core/quality_tier_config.h"
#include "../lod/hlod_auto_route.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/gs_wind_bounds.h"
#include "../renderer/quantization_config.h"
#include "../logger/gs_logger.h"
#include "../io/gaussian_splat_world_io.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/templates/hash_map.h"
// #862: the world-resource resubmit is posted to the MAIN message queue
// explicitly, never to the thread-local override a threaded load installs.
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/renderer_rd/storage_rd/gaussian_splat_storage.h"
#include "servers/rendering_server.h"

namespace {

static bool _is_world_debug_enabled() {
    // Canonical gate: all_debug || frame || data, honoring GS_SILENCE_LOGS.
    return gs::settings::is_any_debug_log_enabled();
}

static uint32_t _world_row_wind_mode(GaussianSplatWorld3D::WindMode p_mode) {
    switch (p_mode) {
        case GaussianSplatWorld3D::WIND_MODE_ON: return GaussianSplatSceneDirector::INSTANCE_WIND_INHERIT;
        case GaussianSplatWorld3D::WIND_MODE_OFF: return GaussianSplatSceneDirector::INSTANCE_WIND_FORCE_DISABLED;
        default: return GaussianSplatSceneDirector::INSTANCE_WIND_SEMANTIC_INHERIT;
    }
}

struct IslandPlacement {
    String name;
    String asset_path;
    Transform3D rest_transform;
    int index = 0;
};

static bool _island_vector(const Variant &p_value, Vector3 &r_vector) {
    if (p_value.get_type() == Variant::VECTOR3) {
        r_vector = p_value;
        return r_vector.is_finite();
    }
    if (p_value.get_type() != Variant::ARRAY) { return false; }
    const Array values = p_value;
    if (values.size() != 3) { return false; }
    for (int i = 0; i < 3; ++i) {
        if (values[i].get_type() != Variant::INT && values[i].get_type() != Variant::FLOAT) { return false; }
        r_vector[i] = double(values[i]);
    }
    return r_vector.is_finite();
}

static String _island_entry(const Dictionary &p_entry, const String &p_world_path, IslandPlacement &r_island) {
    if (r_island.name.is_empty() || r_island.name == "." || r_island.name == ".." ||
            r_island.name.validate_node_name() != r_island.name || r_island.name.strip_edges() != r_island.name) {
        return "island_name";
    }
    const Variant bounds_value = p_entry.get("bounds", Variant());
    if (bounds_value.get_type() != Variant::DICTIONARY) { return "island_bounds"; }
    const Dictionary bounds = bounds_value;
    Vector3 position, size;
    if (!_island_vector(bounds.get("position", Variant()), position) || !_island_vector(bounds.get("size", Variant()), size) ||
            size.x < 0 || size.y < 0 || size.z < 0 || !(position + size).is_finite()) { return "island_bounds"; }

    const Variant rest_value = p_entry.get("rest_transform", Variant());
    if (rest_value.get_type() != Variant::ARRAY) { return "island_rest_transform"; }
    const Array rest = rest_value;
    if (rest.size() != 12) { return "island_rest_transform"; }
    for (int i = 0; i < 12; ++i) {
        if (rest[i].get_type() != Variant::INT && rest[i].get_type() != Variant::FLOAT) { return "island_rest_transform"; }
        if (i < 9) { r_island.rest_transform.basis[i / 3][i % 3] = double(rest[i]); }
        else { r_island.rest_transform.origin[i - 9] = double(rest[i]); }
    }
    if (!r_island.rest_transform.is_finite()) { return "island_rest_transform"; }

    const Variant path_value = p_entry.get("asset_path", Variant());
    if (path_value.get_type() != Variant::STRING || String(path_value).is_empty()) { return "island_asset_path"; }
    const String path = String(path_value).replace("\\", "/");
    // B2 writes output_dir.path_join(name + ".gsplatworld"), including absolute
    // filesystem / res:// / user:// paths. Keep those; never interpret relative
    // traversal against the process cwd or outside the world file's directory.
    for (const String &component : path.split("/", false)) {
        if (component == "..") { return "island_asset_path_escape"; }
    }
    if (path.is_absolute_path()) {
        r_island.asset_path = path.simplify_path();
    } else {
        if (p_world_path.is_empty()) { return "island_world_path_missing"; }
        r_island.asset_path = p_world_path.get_base_dir().path_join(path).simplify_path();
    }
    return String();
}

static String _island_error(const IslandPlacement &p_island, const String &p_reason) {
    return vformat("islands[%d] '%s': %s", p_island.index, p_island.name, p_reason);
}

static Vector<IslandPlacement> _read_island_manifest(const Ref<GaussianSplatWorld> &p_world, PackedStringArray &r_errors) {
    r_errors.clear();
    Vector<IslandPlacement> result;
    if (p_world.is_null()) { r_errors.push_back("world: island_world_missing"); return result; }
    const Dictionary metadata = p_world->get_metadata();
    if (!metadata.has("islands")) { return result; }
    const Variant manifest_value = metadata["islands"];
    if (manifest_value.get_type() != Variant::ARRAY) { r_errors.push_back("islands: island_manifest_type"); return result; }
    const Array manifest = manifest_value;
    HashMap<String, int> names;
    // Count before placing anything: neither occurrence of a duplicate may create a node.
    for (const Variant &value : manifest) {
        if (value.get_type() != Variant::DICTIONARY) { continue; }
        const Dictionary entry = value;
        const Variant name = entry.get("name", Variant());
        if (name.get_type() == Variant::STRING) { ++names[String(name)]; }
    }
    for (int i = 0; i < manifest.size(); ++i) {
        IslandPlacement island;
        island.index = i;
        if (manifest[i].get_type() != Variant::DICTIONARY) {
            r_errors.push_back(_island_error(island, "island_entry_type"));
            continue;
        }
        const Dictionary entry = manifest[i];
        const Variant name = entry.get("name", Variant());
        if (name.get_type() == Variant::STRING) { island.name = name; }
        String reason;
        if (name.get_type() != Variant::STRING) { reason = "island_name"; }
        else if (names[island.name] > 1) { reason = "island_duplicate_name"; }
        else { reason = _island_entry(entry, p_world->get_path(), island); }
        if (!reason.is_empty()) { r_errors.push_back(_island_error(island, reason)); continue; }
        result.push_back(island);
    }
    return result;
}

static Error _load_island_asset(const String &p_path, Ref<GaussianSplatAsset> &r_asset) {
    ResourceFormatLoaderGaussianSplatWorld loader;
    Error error = OK;
    const Ref<GaussianSplatWorld> island_world = loader.load_resident(p_path, &error);
    if (error != OK || island_world.is_null()) { return error != OK ? error : ERR_INVALID_DATA; }
    const Ref<::GaussianData> data = island_world->get_gaussian_data();
    if (data.is_null() || data->get_count() == 0) { return ERR_INVALID_DATA; }
    if (!data->all_render_fields_finite()) { return ERR_INVALID_DATA; }
    // GaussianSplatAsset's existing semantic side schema is named gs_ply_semantics
    // for its first producer. Populate that same schema from the v3 payload so the
    // normal asset/node path retains the word, including after scene serialization.
    PackedInt32Array words;
    if (words.resize(data->get_count()) != OK) { return ERR_OUT_OF_MEMORY; }
    for (int i = 0; i < data->get_count(); ++i) { words.set(i, int32_t(data->get_gaussian(i).semantic)); }
    Dictionary side;
    side["semantic_words"] = words;
    data->set_meta("gs_ply_semantics", side);
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    error = asset->populate_from_gaussian_data(data);
    if (error != OK) { return error; }
    Dictionary import_metadata = asset->get_import_metadata();
    const Dictionary island_metadata = island_world->get_metadata();
    if (island_metadata.has("regions")) { import_metadata["regions"] = island_metadata["regions"]; }
    asset->set_import_metadata(import_metadata);
    r_asset = asset;
    return OK;
}

} // namespace

GaussianSplatWorld3D::GaussianSplatWorld3D() { region_overrides.instantiate(); }
GaussianSplatWorld3D::~GaussianSplatWorld3D() = default;

Ref<GaussianSplatRenderer> GaussianSplatWorld3D::get_renderer() const {
    return renderer;
}

PackedStringArray GaussianSplatWorld3D::get_island_names() {
    const Vector<IslandPlacement> islands = _read_island_manifest(world, island_errors);
    PackedStringArray names;
    for (const IslandPlacement &island : islands) { names.push_back(island.name); }
    return names;
}

Array GaussianSplatWorld3D::create_island_nodes(Node *p_parent) {
    Array created;
    ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), created, "Island nodes must be created on the main thread.");
    const Vector<IslandPlacement> islands = _read_island_manifest(world, island_errors);
    Node *parent = p_parent ? p_parent : this;
    for (const IslandPlacement &island : islands) {
        bool exists = false;
        for (int i = 0; i < parent->get_child_count(true); ++i) {
            if (parent->get_child(i, true)->get_name() == StringName(island.name)) { exists = true; break; }
        }
        if (exists) { continue; } // Idempotent; never rename, modify or replace a pre-existing child.
        if (!FileAccess::exists(island.asset_path)) {
            island_errors.push_back(_island_error(island, "island_asset_missing: " + island.asset_path));
            continue;
        }
        Ref<GaussianSplatAsset> asset;
        const Error error = _load_island_asset(island.asset_path, asset);
        if (error != OK) {
            island_errors.push_back(_island_error(island, vformat("island_asset_unreadable: %s (Error %d)", island.asset_path, int(error))));
            continue;
        }
        GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
        node->set_name(island.name);
        node->set_transform(island.rest_transform);
        node->set_splat_asset(asset);
        // The ordinary node enter-tree path owns instance-tail registration and ledger admission.
        parent->add_child(node);
        if (node->get_parent() != parent) {
            island_errors.push_back(_island_error(island, "island_parent_rejected"));
            memdelete(node);
            continue;
        }
        created.push_back(node);
    }
    return created;
}

void GaussianSplatWorld3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_region_id", "name"), &GaussianSplatWorld3D::get_region_id);
    ClassDB::bind_method(D_METHOD("get_region_names"), &GaussianSplatWorld3D::get_region_names);
    ClassDB::bind_method(D_METHOD("set_region_override", "region", "override"), &GaussianSplatWorld3D::set_region_override);
    ClassDB::bind_method(D_METHOD("clear_region_override", "region"), &GaussianSplatWorld3D::clear_region_override);
    ClassDB::bind_method(D_METHOD("clear_all_region_overrides"), &GaussianSplatWorld3D::clear_all_region_overrides);
    ClassDB::bind_method(D_METHOD("set_tint", "color", "strength"), &GaussianSplatWorld3D::set_tint);
    ClassDB::bind_method(D_METHOD("set_emissive", "color"), &GaussianSplatWorld3D::set_emissive);
    ClassDB::bind_method(D_METHOD("set_hidden", "hidden"), &GaussianSplatWorld3D::set_hidden);

    ClassDB::bind_method(D_METHOD("set_world", "world"), &GaussianSplatWorld3D::set_world);
    ClassDB::bind_method(D_METHOD("get_world"), &GaussianSplatWorld3D::get_world);
    ClassDB::bind_method(D_METHOD("set_auto_apply_on_ready", "enabled"), &GaussianSplatWorld3D::set_auto_apply_on_ready);
    ClassDB::bind_method(D_METHOD("is_auto_apply_on_ready"), &GaussianSplatWorld3D::is_auto_apply_on_ready);
    ClassDB::bind_method(D_METHOD("set_cast_shadow", "enabled"), &GaussianSplatWorld3D::set_cast_shadow);
    ClassDB::bind_method(D_METHOD("is_cast_shadow"), &GaussianSplatWorld3D::is_cast_shadow);
    ClassDB::bind_method(D_METHOD("set_wind_mode", "mode"), &GaussianSplatWorld3D::set_wind_mode);
    ClassDB::bind_method(D_METHOD("get_wind_mode"), &GaussianSplatWorld3D::get_wind_mode);
    BIND_ENUM_CONSTANT(WIND_MODE_AUTO);
    BIND_ENUM_CONSTANT(WIND_MODE_ON);
    BIND_ENUM_CONSTANT(WIND_MODE_OFF);
    ClassDB::bind_method(D_METHOD("set_wind_intensity", "intensity"), &GaussianSplatWorld3D::set_wind_intensity);
    ClassDB::bind_method(D_METHOD("get_wind_intensity"), &GaussianSplatWorld3D::get_wind_intensity);
    ClassDB::bind_method(D_METHOD("set_effectors_enabled", "enabled"), &GaussianSplatWorld3D::set_effectors_enabled);
    ClassDB::bind_method(D_METHOD("is_effectors_enabled"), &GaussianSplatWorld3D::is_effectors_enabled);
    ClassDB::bind_method(D_METHOD("set_lod_enabled", "enabled"), &GaussianSplatWorld3D::set_lod_enabled);
    ClassDB::bind_method(D_METHOD("is_lod_enabled"), &GaussianSplatWorld3D::is_lod_enabled);
    ClassDB::bind_method(D_METHOD("set_hlod_tau_px", "tau"), &GaussianSplatWorld3D::set_hlod_tau_px);
    ClassDB::bind_method(D_METHOD("get_hlod_tau_px"), &GaussianSplatWorld3D::get_hlod_tau_px);
    ClassDB::bind_method(D_METHOD("set_lod_bias", "bias"), &GaussianSplatWorld3D::set_lod_bias);
    ClassDB::bind_method(D_METHOD("get_lod_bias"), &GaussianSplatWorld3D::get_lod_bias);
    ClassDB::bind_method(D_METHOD("set_max_render_distance", "distance"), &GaussianSplatWorld3D::set_max_render_distance);
    ClassDB::bind_method(D_METHOD("get_max_render_distance"), &GaussianSplatWorld3D::get_max_render_distance);
    ClassDB::bind_method(D_METHOD("set_max_splat_count", "count"), &GaussianSplatWorld3D::set_max_splat_count);
    ClassDB::bind_method(D_METHOD("get_max_splat_count"), &GaussianSplatWorld3D::get_max_splat_count);
    ClassDB::bind_method(D_METHOD("set_use_frustum_culling", "enabled"), &GaussianSplatWorld3D::set_use_frustum_culling);
    ClassDB::bind_method(D_METHOD("is_frustum_culling_enabled"), &GaussianSplatWorld3D::is_frustum_culling_enabled);
    ClassDB::bind_method(D_METHOD("set_async_upload_enabled", "enabled"), &GaussianSplatWorld3D::set_async_upload_enabled);
    ClassDB::bind_method(D_METHOD("is_async_upload_enabled"), &GaussianSplatWorld3D::is_async_upload_enabled);
    ClassDB::bind_method(D_METHOD("set_opacity", "opacity"), &GaussianSplatWorld3D::set_opacity);
    ClassDB::bind_method(D_METHOD("get_opacity"), &GaussianSplatWorld3D::get_opacity);
    ClassDB::bind_method(D_METHOD("apply_world"), &GaussianSplatWorld3D::apply_world);
    ClassDB::bind_method(D_METHOD("clear_world"), &GaussianSplatWorld3D::clear_world);
    ClassDB::bind_method(D_METHOD("create_island_nodes", "parent"), &GaussianSplatWorld3D::create_island_nodes, DEFVAL(Variant()));
    ClassDB::bind_method(D_METHOD("get_island_names"), &GaussianSplatWorld3D::get_island_names);
    ClassDB::bind_method(D_METHOD("get_island_errors"), &GaussianSplatWorld3D::get_island_errors);
    ClassDB::bind_method(D_METHOD("get_renderer"), &GaussianSplatWorld3D::get_renderer);

    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "world", PROPERTY_HINT_RESOURCE_TYPE, "GaussianSplatWorld"),
            "set_world", "get_world");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_apply_on_ready"), "set_auto_apply_on_ready", "is_auto_apply_on_ready");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadow"), "set_cast_shadow", "is_cast_shadow");
    ADD_PROPERTY(PropertyInfo(Variant::INT, gs::settings::WORLD_WIND_MODE_PROPERTY, PROPERTY_HINT_ENUM, "Auto,On,Off"), "set_wind_mode", "get_wind_mode");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, gs::settings::WORLD_WIND_INTENSITY_PROPERTY, PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"),
            "set_wind_intensity", "get_wind_intensity");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "effectors_enabled"), "set_effectors_enabled", "is_effectors_enabled");

    ADD_GROUP("Quality", "quality/");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "quality/hlod_tau_px", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater"),
            "set_hlod_tau_px", "get_hlod_tau_px");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "quality/lod_enabled"), "set_lod_enabled", "is_lod_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "quality/lod_bias", PROPERTY_HINT_RANGE, GS_LOD_BIAS_HINT_RANGE),
            "set_lod_bias", "get_lod_bias");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "quality/max_render_distance", PROPERTY_HINT_RANGE, "0.0,10000.0,1.0,or_greater,suffix:m"),
            "set_max_render_distance", "get_max_render_distance");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "quality/max_splat_count", PROPERTY_HINT_RANGE, "1000,10000000,1000"),
            "set_max_splat_count", "get_max_splat_count");

    ADD_GROUP("Rendering", "rendering/");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rendering/frustum_culling"),
            "set_use_frustum_culling", "is_frustum_culling_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rendering/async_upload_enabled"),
            "set_async_upload_enabled", "is_async_upload_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rendering/opacity", PROPERTY_HINT_RANGE, "0.0,8.0,0.01"),
            "set_opacity", "get_opacity");
}

void GaussianSplatWorld3D::_notification(int p_what) {
    const bool log_enabled = _is_world_debug_enabled();
    if (log_enabled) {
        GS_LOG_RENDERER_DEBUG(vformat("[GSWORLD-DBG] _notification p_what=%d", p_what));
    }
    switch (p_what) {
        case NOTIFICATION_ENTER_TREE: {
            if (RenderingServer *rs = RS::get_singleton()) {
                const Callable callback = callable_mp(this, &GaussianSplatWorld3D::_update_deformation_visibility_margin);
                if (!rs->is_connected(SNAME("frame_pre_draw"), callback)) { rs->connect(SNAME("frame_pre_draw"), callback); }
            }
            // NOTIFICATION_READY only fires once per node lifetime, so a node
            // that is removed and re-added (reparenting is exit+enter) never
            // sees READY again. NOTIFICATION_EXIT_TREE below unregisters the
            // world submission and frees render_instance/gaussian_base, so
            // without this the node stays dark after re-entry until someone
            // calls apply_world() or a property setter runs -- see #517.
            //
            // Gated on was_world_submission_active (not unconditional) so a
            // node's *first* entry still respects auto_apply_on_ready exactly
            // as before: was_world_submission_active only becomes true after
            // a successful _register_shared_renderer(), which on first entry
            // only happens via NOTIFICATION_READY below. Re-entries after a
            // prior successful apply restore that state unconditionally,
            // independent of auto_apply_on_ready -- that flag governs whether
            // to *start* showing the world automatically, not whether an
            // already-active world survives a reparent.
            if (was_world_submission_active) {
                apply_world();
            }
        } break;
        case NOTIFICATION_READY: {
            if (log_enabled) {
                GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] NOTIFICATION_READY received!");
            }
            _ensure_renderer();
            if (log_enabled) {
                GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] _ensure_renderer done");
            }
            if (auto_apply_on_ready) {
                if (log_enabled) {
                    GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] auto_apply_on_ready=true, calling apply_world");
                }
                apply_world();
            }
            if (log_enabled) {
                GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] About to call _update_render_instance");
            }
            _update_render_instance();
            if (OS::get_singleton()->has_feature("headless")) {
                set_process(true);
            }
            if (log_enabled) {
                GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] _update_render_instance returned");
            }
        } break;
        case NOTIFICATION_EXIT_TREE: {
            if (RenderingServer *rs = RS::get_singleton()) {
                const Callable callback = callable_mp(this, &GaussianSplatWorld3D::_update_deformation_visibility_margin);
                if (rs->is_connected(SNAME("frame_pre_draw"), callback)) { rs->disconnect(SNAME("frame_pre_draw"), callback); }
            }
            _unregister_shared_renderer();
            _release_gaussian_base();
            if (render_instance.is_valid()) {
                RS::get_singleton()->free(render_instance);
                render_instance = RID();
            }
        } break;
        case NOTIFICATION_PREDELETE: {
            // Per-instance PREDELETE drops *this* world node's references only.
            // It intentionally does NOT call teardown_world_for_scenario():
            // that is a scenario-wide nuclear teardown which would wipe the
            // SharedWorld entry (instances, world-submission, renderer ref)
            // even when THIS world node is not the active world-submission
            // owner -- e.g. a duplicate/secondary GaussianSplatWorld3D in the
            // same scenario whose submit_world_submission() was rejected by
            // the director's ownership arbitration. In that case the active
            // owner is a still-live peer, and the scenario-wide teardown
            // would drop renderer/submission state for that peer and break
            // rendering until it fully rebuilds state. Codex review comment
            // #3294053937 on PR #387 (the WORLD-node analog of the per-NODE
            // PREDELETE bug fixed in commit d939d11b27).
            //
            // release_world_submission(owner_id) is the per-instance,
            // ownership-aware release path: the director's
            // _find_world_for_world_submission only matches when this
            // instance is the actual owner, so non-owners are a no-op and
            // the active owner's SharedWorld is preserved. When this node
            // IS the owner, release_world_submission restores the renderer
            // and calls _prune_world_if_unused, which checks the renderer's
            // reference count -- combined with the renderer.unref() above,
            // the SharedWorld is reclaimed exactly when the last external
            // Ref-holder leaves (the F6 reload intent that motivated the
            // scenario-wide teardown originally; see PR 4 of #352).
            //
            // last_known_scenario is no longer needed for teardown
            // (release_world_submission resolves the scenario from the
            // owner id), but cleared for parity with the node-side fix.
            //
            // Ordering matters here. NOTIFICATION_EXIT_TREE already ran
            // _unregister_shared_renderer() -> release_world_submission(),
            // which cleared the world_submission record and called
            // _prune_world_if_unused. At that point this node still held
            // its `renderer` member Ref so _should_prune_world saw
            // refcount>1 and skipped the prune. We now drop our renderer
            // Ref first so the refcount actually falls when we explicitly
            // re-run the prune below. The second _unregister_shared_renderer()
            // call is a no-op for prune purposes -- the owner record was
            // cleared in EXIT_TREE so release_world_submission's
            // _find_world_for_world_submission() returns null and never
            // reaches _prune_world_if_unused, which is exactly the bug
            // Codex review comment #3294797697 on PR #387 flagged. Without
            // the explicit try_prune_world_if_unused() the SharedWorld
            // lingers across reload cycles holding the renderer/data
            // lifetime anchor, defeating the F6-reload-leak fix that
            // motivated the scenario-wide teardown originally.
            //
            // #862: drop the `changed` connection to the assigned resource.
            //
            // REDUNDANT WITH ~Object(), which tears down every connection this
            // object participates in, and therefore NOT pinned by any test -- the
            // lifecycle case in test_gaussian_splat_node.h says so explicitly
            // rather than pretending to cover it. It is kept for the one window
            // ~Object() does not cover: between this notification and the
            // destructor the node is still in ObjectDB and still reachable from
            // the resource, so a `changed` emitted in that window (a threaded load
            // finishing on a worker thread, which calls connected callables
            // synchronously -- core/io/resource_loader.cpp:965-974) would reach a
            // node that has already released its director submission below.
            //
            // A resubmit already queued on the main MessageQueue needs no such
            // care: flush() skips a call whose target object is gone
            // (core/object/message_queue.cpp:260-270).
            _set_world_resource_changed_connection(world, false);
            renderer.unref();
            _unregister_shared_renderer();
            if (GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton()) {
                director->try_prune_world_if_unused(last_known_scenario);
            }
            last_known_scenario = RID();
        } break;
        // No NOTIFICATION_TRANSFORM_CHANGED case: this node does not call
        // set_notify_transform(true), so Godot never delivers it, and that is
        // intentional rather than an oversight (see #516). GaussianSplatWorld
        // resources are authored in world space -- _apply_world_internal()
        // above already treats a non-identity *global* transform as a
        // compat-warned (or, under
        // rendering/gaussian_splatting/world/strict_identity_transform,
        // hard-rejected) misconfiguration, captured once at apply time
        // (NOTIFICATION_READY / apply_world() / set_world()). Wiring live
        // transform tracking would not fix that contract violation -- the
        // GPU-submitted splat data stays fixed at its world-authored
        // coordinates regardless, so continuously re-deriving local_aabb's
        // world-space transform from a moving node would only walk the
        // render instance's culling volume away from where the content
        // actually is. A node that stays at identity (the supported,
        // documented case) never needs a transform update in the first
        // place. If this contract changes, revisit together with the
        // strict_identity_transform gate and the warned_non_identity_transform
        // one-shot warning below.
        case NOTIFICATION_VISIBILITY_CHANGED: {
            _update_render_instance();
        } break;
        case NOTIFICATION_PROCESS: {
            _notification_process();
        } break;
        default:
            break;
    }
}

void GaussianSplatWorld3D::_notification_process() {
    if (!OS::get_singleton()->has_feature("headless")) {
        return;
    }

    _ensure_renderer();
    if (!renderer.is_valid()) {
        return;
    }

    Viewport *viewport = get_viewport();
    if (!viewport) {
        return;
    }

    Camera3D *camera = viewport->get_camera_3d();
    Transform3D camera_to_world_transform = camera ? camera->get_camera_transform() : get_global_transform();
    Projection camera_projection;
    if (camera) {
        camera_projection = camera->get_camera_projection();
    } else {
        const Size2i viewport_size = viewport->get_visible_rect().size;
        const float aspect = viewport_size.y > 0 ? viewport_size.x / (float)viewport_size.y : 1.0f;
        camera_projection.set_perspective(60.0f, aspect, 0.1f, 1000.0f);
    }

    renderer->set_camera_transform(camera_to_world_transform);
    renderer->set_camera_projection(camera_projection);
    renderer->tick_streaming_only(camera_to_world_transform, camera_projection);
}

void GaussianSplatWorld3D::set_world(const Ref<GaussianSplatWorld> &p_world) {
    // #862: the director stores a *snapshot* of the resource's payload. The Refs
    // this node hands to submit_world_submission() in _register_shared_renderer()
    // below are copied into the SharedWorld's WorldSubmissionRecord by
    // SubmissionStore::store_submission()
    // (../core/gaussian_splat_scene_director.cpp:828-841), and every consumer --
    // record_has_renderable_payload() at :843-851, the route gate
    // get_submission_residency_hint_for_renderer() at :2811-2827 -- reads that
    // record, never the resource. So a mutation of a resource that is already
    // assigned and already applied is invisible until something re-registers.
    // Tracking `changed` is what makes it visible; the connection lives exactly as
    // long as this resource is the assigned one.
    if (world != p_world) {
        _set_world_resource_changed_connection(world, false);
    }
    world = p_world;
    _set_world_resource_changed_connection(world, true);
    bounds_dirty = true;
    if (is_inside_tree()) {
        apply_world();
    }
}

void GaussianSplatWorld3D::_set_world_resource_changed_connection(const Ref<GaussianSplatWorld> &p_world, bool p_connect) {
    if (p_world.is_null()) {
        return;
    }
    // Resource::connect_changed / disconnect_changed rather than a bare
    // connect("changed", ...): they are idempotent, and they route the call
    // through ResourceLoader when a threaded load is in progress on a worker
    // thread (core/io/resource.cpp:199-219), which is exactly the load-order case
    // a GaussianSplatWorld deserialized off the main thread hits.
    const Callable changed_callable = callable_mp(this, &GaussianSplatWorld3D::_on_world_resource_changed);
    if (p_connect) {
        p_world->connect_changed(changed_callable);
    } else {
        p_world->disconnect_changed(changed_callable);
    }
}

void GaussianSplatWorld3D::_on_world_resource_changed() {
    // DEFERRED AND COALESCED, deliberately. Three reasons, in order of weight:
    //
    // 1. Consecutive content writes in one frame (e.g. set_gaussian_data() then
    //    set_bounds()) each emit `changed`. set_gaussian_data() itself emits once
    //    since #1280 (it assigns the payload and its metadata under
    //    render_input_mutex). A synchronous handler would run the whole
    //    registration once per write, and the first run would read the resource
    //    midway through the caller's sequence of writes.
    // 2. submit_world_submission()'s phase 2 is a blocking render-thread dispatch
    //    (../core/gaussian_splat_scene_director.cpp:2425-2430). Running it inline
    //    from inside a property setter puts a render-thread round trip at an
    //    arbitrary point in the writer's code, once per emission.
    // 3. `changed` can arrive off the main thread. Resource::emit_changed()
    //    (core/io/resource.cpp:44-51) hands the emission to
    //    ResourceLoader::resource_changed_emit() while a threaded load is running
    //    on a worker thread, and that calls the connected callable SYNCHRONOUSLY
    //    on the loader thread (core/io/resource_loader.cpp:965-974).
    //
    // MessageQueue::get_main_singleton(), NOT Callable::call_deferred(). The
    // latter posts to MessageQueue::get_singleton(), which is a `thread_local`
    // override (core/object/message_queue.h:161-166) that
    // ResourceLoader::_run_load_task installs for a loader thread and flushes on
    // THAT thread before the load returns (core/io/resource_loader.cpp:380-398).
    // Deferring through it would therefore run the whole registration -- and its
    // blocking render-thread dispatch -- on a loader thread. Posting to the main
    // queue explicitly is what actually makes point 3 true.
    //
    // WHICH EMISSIONS RESUBMIT: all of them, because filtering buys nothing here.
    // Six of the resource's seven emitters -- set_gaussian_data (:120), set_bounds
    // (:129), set_metadata (:135), set_static_chunks (:141),
    // set_chunk_payload_source (:158) and clear() (:321, once, after it has reset
    // all five fields; #1002) -- write a field that store_submission()
    // copies into the record (gaussian_data/payload_source/static_chunks/bounds/
    // metadata, ../core/gaussian_splat_scene_director.cpp:831-835). The seventh,
    // set_payload_metadata(), writes only the resource's scalar metadata
    // mirrors; it can now fire on its own (set_gaussian_data() no longer calls
    // it), so a redundant resubmit is possible. With the coalescing
    // below, the redundant emission costs one latch test.
    //
    // KNOWN GAP, not covered by this: get_metadata() hands script the resource's
    // own Dictionary, so mutating it in place emits nothing and the director keeps
    // the previous metadata until something else re-registers.
    // Detached resource inspection can run without scene services. Do not arm
    // the coalescing latch when there is no queue to clear it later. A later
    // apply_world() captures the resource's current metadata. This callback can
    // run on a loader thread, so leave scene-tree inspection to the flush.
    CallQueue *queue = MessageQueue::get_main_singleton();
    if (!queue) {
        return;
    }
    if (world_resource_resubmit_pending.exchange(true)) {
        return;
    }
    queue->push_callable(
            callable_mp(this, &GaussianSplatWorld3D::_flush_world_resource_resubmit));
}

void GaussianSplatWorld3D::_flush_world_resource_resubmit() {
    // Cleared first so a `changed` emitted by anything below re-arms rather than
    // being swallowed. MessageQueue::flush() skips a queued call whose target has
    // been freed (core/object/message_queue.cpp:260-270), so a node deleted with a
    // resubmit in flight is not a hazard.
    world_resource_resubmit_pending.store(false);

    // Only touch this node's cached bounds and render instance when a resubmit
    // actually happened. A node that was never applied (auto_apply_on_ready =
    // false), was explicitly cleared, or is out of the tree must be left exactly
    // as it is: this handler reacts to content changes on behalf of a live
    // submission, it is not an implicit apply_world().
    if (!_resubmit_world_submission_if_registered()) {
        return;
    }
#ifdef TESTS_ENABLED
    // Past the guard deliberately: this counts resubmits, not flushes. See the
    // member's declaration.
    world_resource_resubmit_run_count++;
#endif

    // The payload moved, so the cached AABB derived from it
    // (world->get_bounds(), else the GaussianData's own AABB) is stale. Without
    // this the render instance keeps the AABB it was given at apply time -- for
    // the empty-then-filled case that is a zero AABB, which clears the custom
    // AABB and leaves the instance culled even though the director now has the
    // payload. Same tail as _apply_world_internal() below.
    bounds_dirty = true;
    _update_bounds();
    _update_render_instance();
}

void GaussianSplatWorld3D::set_auto_apply_on_ready(bool p_enabled) {
    auto_apply_on_ready = p_enabled;
}

void GaussianSplatWorld3D::set_cast_shadow(bool p_enabled) {
    cast_shadow = p_enabled;
    _update_render_instance();
    // Godot's per-instance caster list cannot tell the world from its node instances (one shared renderer), so the HLOD
    // shadow casters read the world's own flag from its submission. Updated in place: no resubmission, no rebuild.
    if (GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton()) {
        director->set_world_submission_casts_shadow(get_instance_id(), p_enabled);
    }
}

void GaussianSplatWorld3D::set_lod_enabled(bool p_enabled) {
    lod_enabled = p_enabled;
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_wind_mode(WindMode p_mode) {
    ERR_FAIL_COND(p_mode < WIND_MODE_AUTO || p_mode > WIND_MODE_OFF);
    if (wind_mode == p_mode) { return; }
    wind_mode = p_mode;
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_wind_intensity(float p_intensity) {
    const float intensity = MAX(0.0f, p_intensity);
    if (Math::is_equal_approx(wind_intensity, intensity)) { return; }
    wind_intensity = intensity;
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_effectors_enabled(bool p_enabled) {
    if (effectors_enabled == p_enabled) { return; }
    effectors_enabled = p_enabled;
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_lod_bias(float p_bias) {
    lod_bias = CLAMP(p_bias, gs::GS_LOD_BIAS_MIN, gs::GS_LOD_BIAS_MAX);
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_max_render_distance(float p_distance) {
    max_render_distance = MAX(0.0f, p_distance);
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_max_splat_count(int p_count) {
    max_splat_count = MAX(gs::GS_MIN_MAX_SPLAT_COUNT, p_count);
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_use_frustum_culling(bool p_enabled) {
    use_frustum_culling = p_enabled;
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_async_upload_enabled(bool p_enabled) {
    async_upload_enabled = p_enabled;
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_opacity(float p_opacity) {
    opacity = CLAMP(p_opacity, 0.0f, 8.0f);
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::apply_world() {
    // Applying the world again is what re-promotes a demoted world to the automatic HLOD route.
    hlod_auto_route_demoted_cause = String();
    _ensure_renderer();
    _apply_world_internal();
}

void GaussianSplatWorld3D::clear_world() {
    _unregister_shared_renderer();
    // Explicit user intent to deactivate: unlike EXIT_TREE's teardown (which
    // must be transparently restored on tree re-entry, see #517), this is a
    // deliberate "stop showing a world" call and must NOT be auto-resumed on
    // the next NOTIFICATION_ENTER_TREE.
    was_world_submission_active = false;
    local_aabb = AABB();
    world_aabb = AABB();
    bounds_dirty = false;
    _update_render_instance();
}

void GaussianSplatWorld3D::_ensure_renderer() {
    _ensure_gaussian_base();

    // Cache the scenario for PREDELETE. get_world_3d() typically returns null
    // by the time PREDELETE fires (the node has left its world ancestor), so
    // we record the scenario at the first opportunity we have a valid one.
    // See last_known_scenario in the header for rationale.
    Ref<World3D> resolved_world = get_world_3d();
    if (resolved_world.is_valid()) {
        last_known_scenario = resolved_world->get_scenario();
    }

    if (!renderer.is_valid()) {
        GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
        if (director) {
            renderer = director->get_shared_renderer(resolved_world.ptr());
        }
    }

    _sync_gaussian_storage();
}

Dictionary GaussianSplatWorld3D::_build_desired_renderer_overrides() const {
    Dictionary overrides;
    overrides[StringName("lod_enabled")] = lod_enabled;
    overrides[StringName("lod_bias")] = lod_bias;
    overrides[StringName("lod_max_distance")] = max_render_distance;
    overrides[StringName("max_splats")] = int64_t(max_splat_count);
    overrides[StringName("frustum_culling")] = use_frustum_culling;
    overrides[StringName("async_upload_enabled")] = async_upload_enabled;
    overrides[StringName("opacity_multiplier")] = (double)opacity;

    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return overrides;
    }

    const String tier_preset = ps->get_setting("rendering/gaussian_splatting/quality/tier_preset", "custom");
    const bool apply_tier_budgets = ps->get_setting("rendering/gaussian_splatting/quality/tier_apply_streaming_budgets", true);
    if (!apply_tier_budgets) {
        return overrides;
    }

    QualityTierConfig tier_config;
    if (!get_quality_tier_config(tier_preset, tier_config)) {
        return overrides;
    }

    const int desired_max_splats = max_splat_count > 0 ? max_splat_count : int(tier_config.max_splats);
    overrides[StringName("max_splats")] = int64_t(MAX(gs::GS_MIN_MAX_SPLAT_COUNT, MIN(int(tier_config.max_splats), desired_max_splats)));

    Dictionary streaming_overrides;
    streaming_overrides[StringName("override_prefetch")] = true;
    streaming_overrides[StringName("predictive_prefetch_enabled")] = ps->get_setting(
            "rendering/gaussian_splatting/streaming/predictive_prefetch_enabled", true);
    float base_prefetch = ps->get_setting(
            "rendering/gaussian_splatting/streaming/prefetch_lookahead_distance", 10.0f);
    float max_distance = max_render_distance;
    float computed_prefetch = max_distance > 0.0f ? max_distance * tier_config.load_ahead_factor : base_prefetch;
    streaming_overrides[StringName("prefetch_lookahead_distance")] = (double)MAX(0.0f, computed_prefetch);

    streaming_overrides[StringName("override_vram_budget")] = true;
    VRAMBudgetConfig vram_config = VRAMBudgetConfig::load_from_project_settings();
    // Tier caps the budget (acts as maximum), but respect project settings if lower
    // Ensure minimum of 64MB to avoid degenerate configurations
    vram_config.budget_mb = MAX(64u, MIN(vram_config.budget_mb, tier_config.max_gpu_memory_mb));
    vram_config.min_chunks = MIN(vram_config.min_chunks, vram_config.max_chunks);
    streaming_overrides[StringName("vram_budget_mb")] = int64_t(vram_config.budget_mb);
    streaming_overrides[StringName("vram_min_chunks")] = int64_t(vram_config.min_chunks);
    streaming_overrides[StringName("vram_max_chunks")] = int64_t(vram_config.max_chunks);

    const String world_path = world.is_valid() ? world->get_path() : String();
    if (!world_path.is_empty() && world_path.get_extension().to_lower() == "gsplatworld") {
        streaming_overrides[StringName("override_io_source")] = true;
    }

    overrides[StringName("streaming")] = streaming_overrides;
    return overrides;
}

bool GaussianSplatWorld3D::_resubmit_world_submission_if_registered() {
    if (!is_inside_tree() || world.is_null()) {
        return false;
    }

    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director) {
        return false;
    }

    // Keyed on THIS node's instance id, so a resource shared by several world
    // nodes fans `changed` out to all of them and each one re-registers only its
    // own submission. A peer that lost scenario arbitration holds no record and
    // stops here; the owner's re-registration goes straight through
    // submit_world_submission() as a same-owner overwrite
    // (../core/gaussian_splat_scene_director.cpp:2391-2401) -- never a
    // release-then-resubmit, so the scenario slot is not surrendered for a peer
    // to take.
    GaussianSplatSceneDirector::WorldSubmission active_submission;
    if (!director->get_world_submission(get_instance_id(), &active_submission)) {
        return false;
    }

    _register_shared_renderer();
    return true;
}

void GaussianSplatWorld3D::_apply_world_internal() {
    const bool log_enabled = _is_world_debug_enabled();
    if (log_enabled) {
        GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] _apply_world_internal ENTER");
    }
    if (world.is_null()) {
        clear_world();
        return;
    }

    // GaussianSplatWorld assets are authored in world space, so any non-identity
    // transform on this node silently misplaces splats. The default policy is a
    // one-shot warning (compat). Setting
    // `rendering/gaussian_splatting/world/strict_identity_transform` to true
    // promotes the check to a hard-fail and skips registration entirely.
    const bool has_non_identity_transform =
            !get_global_transform().is_equal_approx(Transform3D());
    if (has_non_identity_transform) {
        const bool strict_identity = gs::settings::get_bool(
                ProjectSettings::get_singleton(),
                "rendering/gaussian_splatting/world/strict_identity_transform",
                false);
        if (strict_identity) {
            ERR_PRINT("GaussianSplatWorld3D: non-identity transform rejected under "
                      "'rendering/gaussian_splatting/world/strict_identity_transform'. "
                      "World assets are authored in world space; apply the transform "
                      "to the world resource instead. Skipping submission.");
            clear_world();
            return;
        }
    }

    if (log_enabled) {
        GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] _apply_world_internal: calling _register_shared_renderer");
    }
	_register_shared_renderer();
	if (log_enabled) {
		GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] _apply_world_internal: _register_shared_renderer done");
	}
	// clear_world() resets bounds_dirty=false; force a fresh bounds rebuild when
	// re-applying world data so the render instance AABB is restored correctly.
	bounds_dirty = true;
	_update_bounds();
	if (log_enabled) {
		GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] _apply_world_internal: _update_bounds done, calling _update_render_instance");
	}
    _update_render_instance();
    if (log_enabled) {
        GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] _apply_world_internal: _update_render_instance done");
    }

    if (!warned_non_identity_transform && has_non_identity_transform) {
        warned_non_identity_transform = true;
        GS_LOG_WARN_DEFAULT("GaussianSplatWorld3D: non-identity transform detected. World assets are assumed to be in world space.");
    }
}

const char *GaussianSplatWorld3D::_hlod_auto_route_refusal() const {
    // The same domains as GaussianSplatRenderer::hlod_streaming_unsupported_reason() and the paged fallback below. The
    // renderer-held ones are only known once this node has its renderer; later changes are the publisher's (per frame).
    if (!lod_enabled) {
        return "lod_disabled";
    }
    if (renderer.is_valid()) {
        if (renderer->get_painterly_config().enabled) {
            return "painterly_enabled";
        }
        if (renderer->is_shadow_instance_filter_enabled()) {
            return "shadow_instance_filter";
        }
    }
    return nullptr;
}

bool GaussianSplatWorld3D::_mark_hlod_auto_route_demoted(const String &p_cause) {
    if (!hlod_auto_route_demoted_cause.is_empty() || p_cause.is_empty() || !is_inside_tree() || world.is_null()) {
        return false;
    }
    if (world->has_hlod_paged_source()) {
        // MF-1 (ADR beyond-VRAM I-8): a demoted paged world loads its payload into memory. Never beyond the RAM budget: over
        // it the demotion is refused and the world keeps its HLOD route; the refused frames draw no world splats, with their
        // reason, and the route serves the world again once the setup ends (no permanent loss of the main view).
        const uint64_t budget = gs::settings::get_hlod_paged_materialize_ram_budget_bytes(ProjectSettings::get_singleton());
        uint64_t peak = 0;
        if (!world->get_paged_materialize_peak_bytes(peak) || peak > budget) {
            if (hlod_paged_materialize_refused_warned != p_cause) {
                hlod_paged_materialize_refused_warned = p_cause;
                WARN_PRINT(vformat("[GaussianSplatWorld3D] '%s' stays on the HLOD route (%s): leaving it would load the V4 paged world "
                                   "into memory (peak %d MiB), above rendering/gaussian_splatting/lod/hlod_paged_materialize_ram_budget_mb "
                                   "(%d MiB). Frames the route refuses draw no world splats (render stat hlod_reason).",
                        String(get_path()), p_cause, int64_t(peak >> 20), int64_t(budget >> 20)));
            }
            return false;
        }
    }
    hlod_auto_route_demoted_cause = p_cause;
    WARN_PRINT(vformat("[GaussianSplatWorld3D] '%s' leaves the HLOD route (%s): the route cannot serve this setup. It now draws "
                       "on the route it has without the automatic HLOD route (a V4 paged world loads its payload into memory, within "
                       "lod/hlod_paged_materialize_ram_budget_mb) until apply_world() runs again.", String(get_path()), p_cause));
    return true;
}

void GaussianSplatWorld3D::_on_hlod_auto_route_demoted(const String &p_cause, uint64_t p_generation) {
    // A request for an older submission: the snapshot it refused is gone, and the current one was not refused.
    if (p_generation != hlod_auto_route_submission_generation) {
        return;
    }
    if (_mark_hlod_auto_route_demoted(p_cause)) {
        _resubmit_world_submission_if_registered();
    }
}

void GaussianSplatWorld3D::_register_shared_renderer() {
    if (!is_inside_tree()) {
        return;
    }
    if (world.is_null()) {
        return;
    }
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director) {
        return;
    }

    region_overrides->set_manifest(world->get_metadata());
    GaussianSplatSceneDirector::WorldSubmission submission;
    submission.region_overrides = region_overrides;
    submission.owner_id = get_instance_id();
    submission.casts_shadow = cast_shadow; // Row 0 of the HLOD shadow casters (#1095); see set_cast_shadow().
    submission.wind_intensity = wind_intensity;
    submission.wind_mode = _world_row_wind_mode(wind_mode);
    submission.scene_effectors_enabled = effectors_enabled;
    submission.scenario = get_world_3d().is_valid() ? get_world_3d()->get_scenario() : RID();
    // Refresh the cached scenario so PREDELETE can teardown even after the
    // node has left its world ancestor. See last_known_scenario in the header.
    if (submission.scenario.is_valid()) {
        last_known_scenario = submission.scenario;
    }
    bool paged_fallback_refused = false; // MF-1: the paged fallback below was refused over the RAM budget.
    if (world->has_hlod_paged_source()) {
        // Package E1 fallback: the HLOD runtime route is the only one a V4 paged world can use. Where that route can never
        // serve this setup (and the node can see it now), load the payload into memory - the memory of a v3 load of the
        // same world, within lod/hlod_paged_materialize_ram_budget_mb - and take the ordinary resident route. Per-view
        // limits (orthographic, XR) are not a reason: the route is usable and those frames draw nothing, with a reason key
        // (the renderer never demotes for them either, lod/hlod_auto_route.h).
        String fallback_reason;
        ProjectSettings *ps = ProjectSettings::get_singleton();
        const bool demoted = !hlod_auto_route_demoted_cause.is_empty();
        if (demoted) {
            fallback_reason = String(gs_hlod_auto_route::AUTO_ROUTE_DEMOTED_PREFIX) + hlod_auto_route_demoted_cause;
        } else if (!gs::settings::get_hlod_runtime_cut_enabled(ps)) {
            fallback_reason = "hlod_paged_requires_runtime_cut";
        } else if (!lod_enabled) {
            fallback_reason = "hlod_paged_lod_disabled";
        } else if (get_global_transform() != Transform3D()) {
            fallback_reason = "hlod_paged_non_identity_world_transform";
        } else if (renderer.is_valid() && renderer->get_painterly_config().enabled) {
            fallback_reason = "hlod_paged_painterly_enabled";
        } else if (renderer.is_valid() && renderer->is_shadow_instance_filter_enabled()) {
            fallback_reason = "hlod_paged_shadow_instance_filter";
        }
        if (!fallback_reason.is_empty()) {
            if (!demoted) { // A demotion printed its own warning.
            WARN_PRINT_ONCE(vformat("[GaussianSplatWorld3D] V4 paged world cannot use the HLOD route (%s); loading its payload into memory "
                                    "and using the resident route (the memory of a v3 load).", fallback_reason));
            }
            String materialize_reason;
            // MF-1 (ADR beyond-VRAM I-8): never beyond the explicit RAM budget. Over it the world stays paged and draws no
            // world splats here, with the reason; the HLOD route cannot serve this setup either, so no snapshot is taken.
            const uint64_t budget = gs::settings::get_hlod_paged_materialize_ram_budget_bytes(ps);
            if (world->materialize_paged_resident(budget, materialize_reason) != OK) {
                const bool over_budget = materialize_reason == "hlod_paged_materialize_over_ram_budget";
                uint64_t peak = 0;
                world->get_paged_materialize_peak_bytes(peak);
                ERR_PRINT(vformat("[GaussianSplatWorld3D] Paged-world fallback failed (%s): %s%s", fallback_reason, materialize_reason,
                        over_budget ? vformat(" (peak %d MiB, rendering/gaussian_splatting/lod/hlod_paged_materialize_ram_budget_mb %d MiB); "
                                              "the world draws no splats until the setup changes and apply_world() runs, or the budget is raised",
                                              int64_t(peak >> 20), int64_t(budget >> 20))
                                    : String()));
                submission.hlod_reason = over_budget ? materialize_reason : String("hlod_paged_materialize_failed");
                paged_fallback_refused = over_budget;
            } else {
                submission.hlod_reason = fallback_reason;
            }
        }
    }
    const uint64_t source_epoch = world->capture_render_input(submission.gaussian_data, submission.payload_source,
            submission.static_chunks, submission.bounds, submission.metadata);
    submission.hlod_tau_px = hlod_tau_px;
    submission.hlod_world_key = world.is_valid() ? uint64_t(world->get_instance_id()) : 0;
    // The automatic HLOD route (lod/hlod_auto_route.h, ADR one-route-many-producers 4.7): a world that can carry an HLOD
    // snapshot takes the HLOD route with its node instances, whatever route_policy says; the cut setting is the opt-out.
    ProjectSettings *route_ps = ProjectSettings::get_singleton();
    const bool route_policy_streaming = gs::settings::get_streaming_route_policy(route_ps) == gs::settings::GS_ROUTE_STREAMING;
    gs_hlod_auto_route::WorldInputs route_inputs;
    route_inputs.cut_enabled = gs::settings::get_hlod_runtime_cut_enabled(route_ps);
    route_inputs.has_hlod_tree = world->has_hlod_tree();
    route_inputs.resident_payload = world->has_resident_gaussian_data();
    route_inputs.paged = world->has_hlod_paged_source();
    route_inputs.identity_transform = get_global_transform() == Transform3D();
    route_inputs.route_policy_streaming = route_policy_streaming;
    route_inputs.domain_refusal = _hlod_auto_route_refusal();
    route_inputs.demoted = !hlod_auto_route_demoted_cause.is_empty();
    const gs_hlod_auto_route::WorldPlan route_plan = gs_hlod_auto_route::plan_world(route_inputs);
    if (submission.hlod_reason.is_empty()) {
        submission.hlod_reason = route_inputs.demoted
                ? String(gs_hlod_auto_route::AUTO_ROUTE_DEMOTED_PREFIX) + hlod_auto_route_demoted_cause
                : route_plan.reason; // A paged fallback above keeps its own, more specific reason.
    }
    if (route_plan.capture_snapshot && !paged_fallback_refused) {
        const Error snapshot_error = world->capture_hlod_runtime_snapshot(submission.hlod_snapshot, submission.hlod_reason);
        if (snapshot_error == OK && (submission.hlod_snapshot->source_epoch != source_epoch ||
                submission.hlod_snapshot->source_data != submission.gaussian_data)) {
            submission.hlod_snapshot.unref();
            submission.hlod_reason = "hlod_submission_generation_changed";
        }
        if (snapshot_error != OK) {
            submission.hlod_snapshot.unref();
        }
        // The quantized route refuses a tree with mixed DC encodings (hlod_quantized_mixed_dc_node), and a paged world has
        // no other payload: known now, so demote now instead of drawing nothing first. The demotion resubmits.
        if (submission.hlod_snapshot.is_valid() && g_quantization_config.per_chunk_quantization && !submission.hlod_snapshot->quantized_eligible) {
            submission.hlod_snapshot.unref();
            if (_mark_hlod_auto_route_demoted("quantized_mixed_dc_node")) {
                _register_shared_renderer();
                return;
            }
            if (world->has_hlod_paged_source()) {
                // MF-1: the demotion was refused over the RAM budget (warned there); a paged world has no other payload.
                submission.hlod_reason = "hlod_paged_materialize_over_ram_budget";
            }
        }
    }
    // A world the automatic route concerns but that does not take it says so once per cause (the paged fallback above
    // printed its own warning). The reason is also the render stat hlod_reason.
    if (submission.hlod_snapshot.is_null() && !submission.hlod_reason.is_empty() && !submission.hlod_reason.begins_with("hlod_paged_") &&
            !submission.hlod_reason.begins_with(gs_hlod_auto_route::AUTO_ROUTE_DEMOTED_PREFIX) &&
            submission.hlod_reason != hlod_auto_route_warned_reason) {
        hlod_auto_route_warned_reason = submission.hlod_reason;
        WARN_PRINT(vformat("[GaussianSplatWorld3D] '%s' has an HLOD tree but does not take the HLOD route (%s); it draws on the %s route. "
                           "Set rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled = false to silence this.",
                String(get_path()), submission.hlod_reason, route_policy_streaming ? "chunk-streaming" : "resident"));
    }
    const String world_path = world->get_path();
    if (!world_path.is_empty()) {
        submission.metadata[StringName("world_path")] = world_path;
    }
    // World submissions carry a residency hint. A captured HLOD snapshot takes the HLOD route, which runs on the
    // resident backend; every other world defers to the active route policy. When the policy is STREAMING the hint
    // lets the renderer bootstrap the streaming system; when it is RESIDENT the hint preserves the fully-materialized
    // resident path.
    submission.has_desired_residency_hint = true;
    submission.desired_residency_hint = gs_hlod_auto_route::residency_hint_resident(route_policy_streaming, submission.hlod_snapshot.is_valid())
            ? GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT
            : GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING;
    submission.desired_renderer_overrides = _build_desired_renderer_overrides();
    // Every submission is a new generation, with or without a snapshot: a request still in flight for the previous one
    // is stale from here on.
    ++hlod_auto_route_submission_generation;
    if (submission.hlod_snapshot.is_valid()) {
        submission.hlod_demotion_callback = callable_mp(this, &GaussianSplatWorld3D::_on_hlod_auto_route_demoted)
                .bind(hlod_auto_route_submission_generation);
    }
    if (!director->submit_world_submission(submission)) {
        if (_is_world_debug_enabled()) {
            GS_LOG_RENDERER_DEBUG(vformat("[GSWORLD-DBG] Skipping apply_world: scenario %d ownership is held by another world",
                    (uint64_t)submission.scenario.get_id()));
        }
        return;
    }

    was_world_submission_active = true;
    _ensure_renderer();
}

void GaussianSplatWorld3D::_unregister_shared_renderer() {
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director) {
        return;
    }
    director->release_world_submission(get_instance_id());
}

void GaussianSplatWorld3D::_update_bounds() {
    if (!bounds_dirty) {
        return;
    }

    local_aabb = AABB();
    if (world.is_valid()) {
        local_aabb = world->get_bounds();
        if (!local_aabb.has_volume()) {
            Ref<GaussianData> splat_data = world->get_gaussian_data();
            if (splat_data.is_valid()) {
                local_aabb = splat_data->get_aabb();
            }
        }
    }

    Transform3D xform = get_global_transform();
    world_aabb = local_aabb.has_volume() ? xform.xform(local_aabb) : AABB();
    bounds_dirty = false;
}

void GaussianSplatWorld3D::_update_render_instance() {
    static int update_call_count = 0;
    const bool log_enabled = _is_world_debug_enabled();
    if (log_enabled) {
        GS_LOG_RENDERER_DEBUG(vformat("[GSWORLD-DBG] _update_render_instance CALLED #%d render_instance=%s",
                ++update_call_count, render_instance.is_valid() ? "valid" : "invalid"));
    } else {
        ++update_call_count;
    }
    RenderingServer *rs = RS::get_singleton();
    if (!rs) {
        if (log_enabled) {
            GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] RenderingServer is null");
        }
        return;
    }

    if (!render_instance.is_valid()) {
        render_instance = rs->instance_create();
        deformation_visibility_margin = -1.0;
        rs->instance_attach_object_instance_id(render_instance, get_instance_id());
    }

    if (is_inside_tree()) {
        Ref<World3D> world_3d = get_world_3d();
        if (world_3d.is_valid()) {
            rs->instance_set_scenario(render_instance, world_3d->get_scenario());
        }

        Transform3D xform = get_global_transform();
        rs->instance_set_transform(render_instance, xform);
        rs->instance_set_visible(render_instance, is_visible_in_tree());
    }

    rs->instance_set_custom_aabb(render_instance, local_aabb);
    rs->instance_geometry_set_cast_shadows_setting(render_instance,
            cast_shadow ? RS::SHADOW_CASTING_SETTING_ON : RS::SHADOW_CASTING_SETTING_OFF);

    _ensure_gaussian_base();
    _set_instance_base(gaussian_base);
    _sync_gaussian_storage();
    _update_deformation_visibility_margin();
}

void GaussianSplatWorld3D::_update_deformation_visibility_margin() {
    // RenderingServerDefault::draw emits frame_pre_draw on the main thread BEFORE queueing its cull/draw. A direct
    // callback sees this frame's settings and row/effector edits, even when the static rest AABB is wholly off-screen.
    RenderingServer *rs = RS::get_singleton();
    if (!rs || !is_inside_tree() || !render_instance.is_valid()) { return; }
    InstanceDataGPU row = {};
    row.params[2] = wind_intensity;
    row.params[3] = float(_world_row_wind_mode(wind_mode));
    row.effect_params[0] = effectors_enabled ? 1.0f : 0.0f;
    const double scene_margin = GaussianSplatSceneDirector::deformation_visibility_margin_for_renderer(renderer.ptr(), row);
    const real_t local_margin = gs_wind::local_visibility_margin(scene_margin, get_global_transform().basis);
    if (deformation_visibility_margin != local_margin) {
        rs->instance_set_extra_visibility_margin(render_instance, local_margin);
        deformation_visibility_margin = local_margin;
    }
}

void GaussianSplatWorld3D::_ensure_gaussian_base() {
    if (gaussian_base.is_valid()) {
        return;
    }

    RendererRD::GaussianSplatStorage *storage = RendererRD::GaussianSplatStorage::get_singleton();
    if (!storage) {
        return;
    }

    gaussian_base = storage->gaussian_allocate();
    storage->gaussian_initialize(gaussian_base);
#ifdef MODULE_GAUSSIAN_SPLATTING_ENABLED
    storage->gaussian_set_renderer(gaussian_base, renderer);
#endif
    // Local bounds for the scene cull, which applies the instance transform; world bounds for
    // the HLOD shadow refresh, which holds only the slot (#1548).
    storage->gaussian_set_aabb(gaussian_base, local_aabb);
    storage->gaussian_set_world_aabb(gaussian_base, world_aabb);
    storage->gaussian_set_casts_shadow(gaussian_base, cast_shadow);
}

void GaussianSplatWorld3D::_release_gaussian_base() {
    if (!gaussian_base.is_valid()) {
        return;
    }

    // #1161: same order and routing as GaussianSplatNode3D::_release_gaussian_base
    // (unset base -> clear renderer -> RenderingServer::free). This node used to
    // free the slot while render_instance still named it as its base.
    _set_instance_base(RID());

    RendererRD::GaussianSplatStorage *storage = RendererRD::GaussianSplatStorage::get_singleton();
    if (storage) {
#ifdef MODULE_GAUSSIAN_SPLATTING_ENABLED
        storage->gaussian_set_renderer(gaussian_base, Ref<GaussianSplatRenderer>());
#endif
        if (RenderingServer *rs = RS::get_singleton()) {
            rs->free(gaussian_base);
        } else {
            storage->gaussian_free(gaussian_base);
        }
    }

    gaussian_base = RID();
}

void GaussianSplatWorld3D::_sync_gaussian_storage() {
    if (!gaussian_base.is_valid()) {
        return;
    }

    RendererRD::GaussianSplatStorage *storage = RendererRD::GaussianSplatStorage::get_singleton();
    if (!storage) {
        return;
    }

#ifdef MODULE_GAUSSIAN_SPLATTING_ENABLED
    storage->gaussian_set_renderer(gaussian_base, renderer);
#endif
    // Local bounds for the scene cull, which applies the instance transform; world bounds for
    // the HLOD shadow refresh, which holds only the slot (#1548).
    storage->gaussian_set_aabb(gaussian_base, local_aabb);
    storage->gaussian_set_world_aabb(gaussian_base, world_aabb);
    storage->gaussian_set_casts_shadow(gaussian_base, cast_shadow);
}

void GaussianSplatWorld3D::_set_instance_base(const RID &p_base) {
    const bool log_enabled = _is_world_debug_enabled();
    if (log_enabled) {
        GS_LOG_RENDERER_DEBUG(vformat("[GSWORLD-DBG] _set_instance_base: render_instance=%s p_base=%s",
                render_instance.is_valid() ? "valid" : "invalid",
                p_base.is_valid() ? "valid" : "invalid"));
    }
    if (!render_instance.is_valid()) {
        return;
    }

    RenderingServer *rs = RS::get_singleton();
    if (!rs) {
        return;
    }

    rs->instance_set_base(render_instance, p_base);
    if (log_enabled) {
        GS_LOG_RENDERER_DEBUG("[GSWORLD-DBG] instance_set_base called successfully");
    }
}

void GaussianSplatWorld3D::set_hlod_tau_px(float p_tau) {
    ERR_FAIL_COND_MSG(!Math::is_finite(p_tau) || p_tau < 0.0f, "HLOD pixel threshold must be finite and nonnegative.");
    if (hlod_tau_px == p_tau) {
        return;
    }
    hlod_tau_px = p_tau;
    _resubmit_world_submission_if_registered();
}

int GaussianSplatWorld3D::get_region_id(const StringName &p_name) const { return GSRegionOverrideState::region_id(world.is_valid() ? world->get_metadata() : Dictionary(), p_name); }
PackedStringArray GaussianSplatWorld3D::get_region_names() const { return GSRegionOverrideState::region_names(world.is_valid() ? world->get_metadata() : Dictionary()); }
void GaussianSplatWorld3D::_sync_region_overrides() {
    region_overrides->set_manifest(world.is_valid() ? world->get_metadata() : Dictionary());
    if (auto *director = GaussianSplatSceneDirector::get_singleton()) { director->update_region_overrides(get_instance_id(), region_overrides); }
}
void GaussianSplatWorld3D::set_region_override(int p_region, const Ref<GaussianSplatRegionOverride> &p_override) {
    ERR_FAIL_COND(p_region < 0 || p_region > 65535 || p_override.is_null());
    region_overrides->set_override(p_region, p_override); _sync_region_overrides();
}
void GaussianSplatWorld3D::clear_region_override(int p_region) { region_overrides->clear_override(p_region); _sync_region_overrides(); }
void GaussianSplatWorld3D::clear_all_region_overrides() { region_overrides->clear_all(); _sync_region_overrides(); }
void GaussianSplatWorld3D::set_tint(const Color &p_color, float p_strength) { auto whole = region_overrides->whole_asset(); whole->set_tint(p_color); whole->set_tint_strength(p_strength); _sync_region_overrides(); }
void GaussianSplatWorld3D::set_emissive(const Color &p_color) { region_overrides->whole_asset()->set_emissive(p_color); _sync_region_overrides(); }
void GaussianSplatWorld3D::set_hidden(bool p_hidden) { region_overrides->set_hidden(p_hidden); _sync_region_overrides(); }
