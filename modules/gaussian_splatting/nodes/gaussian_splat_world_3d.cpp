#include "gaussian_splat_world_3d.h"

#include "../core/gaussian_splat_scene_director.h"
#include "../core/gs_project_settings.h"
#include "../core/quality_tier_config.h"
#include "../logger/gs_logger.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/renderer_rd/storage_rd/gaussian_splat_storage.h"
#include "servers/rendering_server.h"

namespace {

static bool _is_world_debug_enabled() {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return false;
    }
    if (ps->get_setting("rendering/gaussian_splatting/debug/enable_all_debug", false)) {
        return true;
    }
    if (ps->get_setting("rendering/gaussian_splatting/debug/enable_frame_logging", false)) {
        return true;
    }
    return ps->get_setting("rendering/gaussian_splatting/debug/enable_data_logging", false);
}

} // namespace

void GaussianSplatWorld3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_world", "world"), &GaussianSplatWorld3D::set_world);
    ClassDB::bind_method(D_METHOD("get_world"), &GaussianSplatWorld3D::get_world);
    ClassDB::bind_method(D_METHOD("set_auto_apply_on_ready", "enabled"), &GaussianSplatWorld3D::set_auto_apply_on_ready);
    ClassDB::bind_method(D_METHOD("is_auto_apply_on_ready"), &GaussianSplatWorld3D::is_auto_apply_on_ready);
    ClassDB::bind_method(D_METHOD("set_cast_shadow", "enabled"), &GaussianSplatWorld3D::set_cast_shadow);
    ClassDB::bind_method(D_METHOD("is_cast_shadow"), &GaussianSplatWorld3D::is_cast_shadow);
    ClassDB::bind_method(D_METHOD("set_lod_enabled", "enabled"), &GaussianSplatWorld3D::set_lod_enabled);
    ClassDB::bind_method(D_METHOD("is_lod_enabled"), &GaussianSplatWorld3D::is_lod_enabled);
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
    ClassDB::bind_method(D_METHOD("get_renderer"), &GaussianSplatWorld3D::get_renderer);

    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "world", PROPERTY_HINT_RESOURCE_TYPE, "GaussianSplatWorld"),
            "set_world", "get_world");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_apply_on_ready"), "set_auto_apply_on_ready", "is_auto_apply_on_ready");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadow"), "set_cast_shadow", "is_cast_shadow");

    ADD_GROUP("Quality", "quality/");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "quality/lod_enabled"), "set_lod_enabled", "is_lod_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "quality/lod_bias", PROPERTY_HINT_RANGE, "0.1,4.0,0.1"),
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
            renderer.unref();
            _unregister_shared_renderer();
            if (GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton()) {
                director->try_prune_world_if_unused(last_known_scenario);
            }
            last_known_scenario = RID();
        } break;
        case NOTIFICATION_TRANSFORM_CHANGED: {
            bounds_dirty = true;
            _update_bounds();
            _update_render_instance();
        } break;
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
    world = p_world;
    bounds_dirty = true;
    if (is_inside_tree()) {
        apply_world();
    }
}

void GaussianSplatWorld3D::set_auto_apply_on_ready(bool p_enabled) {
    auto_apply_on_ready = p_enabled;
}

void GaussianSplatWorld3D::set_cast_shadow(bool p_enabled) {
    cast_shadow = p_enabled;
    _update_render_instance();
}

void GaussianSplatWorld3D::set_lod_enabled(bool p_enabled) {
    lod_enabled = p_enabled;
    _resubmit_world_submission_if_registered();
}

void GaussianSplatWorld3D::set_lod_bias(float p_bias) {
    lod_bias = CLAMP(p_bias, 0.1f, 4.0f);
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
    _ensure_renderer();
    _apply_world_internal();
}

void GaussianSplatWorld3D::clear_world() {
    _unregister_shared_renderer();
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

void GaussianSplatWorld3D::_resubmit_world_submission_if_registered() {
    if (!is_inside_tree() || world.is_null()) {
        return;
    }

    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director) {
        return;
    }

    GaussianSplatSceneDirector::WorldSubmission active_submission;
    if (!director->get_world_submission(get_instance_id(), &active_submission)) {
        return;
    }

    _register_shared_renderer();
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

    GaussianSplatSceneDirector::WorldSubmission submission;
    submission.owner_id = get_instance_id();
    submission.scenario = get_world_3d().is_valid() ? get_world_3d()->get_scenario() : RID();
    // Refresh the cached scenario so PREDELETE can teardown even after the
    // node has left its world ancestor. See last_known_scenario in the header.
    if (submission.scenario.is_valid()) {
        last_known_scenario = submission.scenario;
    }
    submission.gaussian_data = world->get_gaussian_data();
    submission.payload_source = world->get_chunk_payload_source();
    submission.static_chunks = world->get_static_chunks();
    submission.bounds = world->get_bounds();
    if (!submission.bounds.has_volume() && submission.gaussian_data.is_valid()) {
        submission.bounds = submission.gaussian_data->get_aabb();
    }
    submission.metadata = world->get_metadata();
    const String world_path = world->get_path();
    if (!world_path.is_empty()) {
        submission.metadata[StringName("world_path")] = world_path;
    }
    // World submissions carry a residency hint that defers to the active
    // route policy.  When the policy is STREAMING the hint lets the renderer
    // bootstrap the streaming system; when it is RESIDENT the hint preserves
    // the fully-materialized resident path.
    submission.has_desired_residency_hint = true;
    const int route_policy = gs::settings::get_streaming_route_policy(ProjectSettings::get_singleton());
    submission.desired_residency_hint = (route_policy == gs::settings::GS_ROUTE_STREAMING)
            ? GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING
            : GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
    submission.desired_renderer_overrides = _build_desired_renderer_overrides();
    if (!director->submit_world_submission(submission)) {
        if (_is_world_debug_enabled()) {
            GS_LOG_RENDERER_DEBUG(vformat("[GSWORLD-DBG] Skipping apply_world: scenario %d ownership is held by another world",
                    (uint64_t)submission.scenario.get_id()));
        }
        return;
    }

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
    storage->gaussian_set_aabb(gaussian_base, world_aabb);
    storage->gaussian_set_casts_shadow(gaussian_base, cast_shadow);
}

void GaussianSplatWorld3D::_release_gaussian_base() {
    if (!gaussian_base.is_valid()) {
        return;
    }

    RendererRD::GaussianSplatStorage *storage = RendererRD::GaussianSplatStorage::get_singleton();
    if (storage) {
#ifdef MODULE_GAUSSIAN_SPLATTING_ENABLED
        storage->gaussian_set_renderer(gaussian_base, Ref<GaussianSplatRenderer>());
#endif
        storage->gaussian_set_aabb(gaussian_base, AABB());
        storage->gaussian_set_casts_shadow(gaussian_base, false);
        storage->gaussian_free(gaussian_base);
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
    storage->gaussian_set_aabb(gaussian_base, world_aabb);
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
