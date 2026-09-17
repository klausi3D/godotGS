#ifndef GAUSSIAN_SPLAT_WORLD_3D_H
#define GAUSSIAN_SPLAT_WORLD_3D_H

#include "scene/3d/node_3d.h"
#include "core/math/aabb.h"

#include "../core/gaussian_splat_world.h"
#include "../renderer/gaussian_splat_renderer.h"

// #862: the coalescing latch below is armed from whichever thread emitted
// `changed` and cleared from the main thread, so it must be atomic.
#include <atomic>

class GaussianSplatWorld3D : public Node3D {
    GDCLASS(GaussianSplatWorld3D, Node3D);

private:
    Ref<GaussianSplatWorld> world;
    Ref<GaussianSplatRenderer> renderer;

    RID render_instance;
    RID gaussian_base;

    // Scenario RID cached at the moment we first resolve a valid World3D
    // (in _ensure_renderer / _apply_world_internal). NOTIFICATION_PREDELETE
    // can run after the node has left its world ancestor, at which point
    // Node3D::get_world_3d() returns null; without this cache the director
    // entry for world-submission-only scenes would leak because PREDELETE
    // could not resolve the scenario. Mirrors GaussianSplatNode3D's
    // last_known_scenario pattern -- see PR 4 of #352.
    RID last_known_scenario;

    bool auto_apply_on_ready = true;
    bool cast_shadow = false;
    bool bounds_dirty = true;
    bool warned_non_identity_transform = false;

    // Sticky "this node has a live, successfully-registered world submission"
    // flag. Set true when _register_shared_renderer() successfully submits to
    // the director; cleared only by clear_world() (explicit user intent to
    // deactivate). NOTIFICATION_EXIT_TREE deliberately does NOT clear this --
    // it tears down the *live* director/RS state (see _notification's
    // EXIT_TREE case) but does not change what the node was configured to
    // show. NOTIFICATION_ENTER_TREE consults this flag to distinguish a
    // node's first-ever entry (gated by auto_apply_on_ready, as before) from
    // a re-entry after reparenting (exit+enter), where the prior submission
    // must be restored unconditionally -- see issue #517.
    bool was_world_submission_active = false;

    // #862 coalescing latch. True from the moment a `changed` emission from the
    // assigned GaussianSplatWorld has queued a deferred resubmit until that
    // resubmit runs. Further emissions in the same frame fold into the pending
    // run instead of queueing another one -- a single user-visible content write
    // emits `changed` more than once (see _on_world_resource_changed()), and
    // re-running the registration per emission would turn one property write into
    // several render-thread round trips. It is also the re-entrancy guard: a
    // `changed` emitted from inside the flush cannot nest a second registration.
    //
    // ATOMIC, not a plain bool, because the two accesses are not on the same
    // thread. Resource::emit_changed() (core/io/resource.cpp:44-51) routes to
    // ResourceLoader::resource_changed_emit() when a threaded load is running on a
    // worker thread, and that calls the connected callable SYNCHRONOUSLY on the
    // loader thread (core/io/resource_loader.cpp:965-974). So the arm can happen
    // off the main thread while the disarm always happens on it. A plain
    // read-then-write would be an unsynchronized read-modify-write on a shared
    // object; the arm below uses exchange() so two concurrent emissions still
    // queue exactly one flush.
    std::atomic<bool> world_resource_resubmit_pending{ false };

#ifdef TESTS_ENABLED
    // Test-only witness for the coalescing contract above, compiled out of every
    // build without TESTS_ENABLED (release templates included -- #725 shipped test
    // hooks in a release build once). Counts RESUBMITS, not flushes: it is
    // incremented past the already-registered guard, so a flush on a node that
    // holds no submission leaves it alone and a test may read it as "this node
    // resubmitted N times". How many `changed` emissions a resource write produced
    // and how many resubmits they collapsed into is otherwise unobservable from
    // outside the node, so a test asserting "one write, one resubmit" would have
    // nothing to assert and the latch could be deleted with every test still green.
    uint64_t world_resource_resubmit_run_count = 0;
#endif

    AABB local_aabb;
    AABB world_aabb;

    // Renderer parity settings (matches the core controls on GaussianSplatNode3D).
    bool lod_enabled = true;
    float lod_bias = 1.0f;
    float max_render_distance = 1000.0f;
    int max_splat_count = 1000000;
    bool use_frustum_culling = true;
    bool async_upload_enabled = true;
    float opacity = 1.0f;

    void _ensure_renderer();
    Dictionary _build_desired_renderer_overrides() const;
    // Returns true when this node held a live director submission and the
    // re-registration was therefore run. It does NOT report whether the director
    // accepted the resubmission -- _register_shared_renderer() can still lose
    // scenario arbitration -- only whether the "already registered" precondition
    // held. Callers that must not act on an unregistered node key off this.
    bool _resubmit_world_submission_if_registered();
    void _apply_world_internal();
    // #862: keeps the assigned GaussianSplatWorld's `changed` signal connected for
    // exactly as long as it is the assigned resource. A null Ref is a no-op, and
    // both directions are idempotent for every call made on the main thread --
    // which is every call this class makes in practice, because a
    // GaussianSplatWorld3D is constructed by scene instantiation.
    //
    // NOT idempotent across a threaded load: Resource::connect_changed() called on
    // a loader thread only queues the connection into the ThreadLoadTask
    // (core/io/resource_loader.cpp:933-949) and a later disconnect_changed() from
    // the MAIN thread takes the ordinary path, finds nothing connected, and leaves
    // the queued entry to be honoured when the load completes at :894. See #1005.
    void _set_world_resource_changed_connection(const Ref<GaussianSplatWorld> &p_world, bool p_connect);
    void _on_world_resource_changed();
    void _flush_world_resource_resubmit();
    void _register_shared_renderer();
    void _unregister_shared_renderer();
    void _update_bounds();
    void _update_render_instance();
    void _ensure_gaussian_base();
    void _release_gaussian_base();
    void _sync_gaussian_storage();
    void _set_instance_base(const RID &p_base);

    void _notification_process();
protected:
    static void _bind_methods();
    void _notification(int p_what);

public:
    void set_world(const Ref<GaussianSplatWorld> &p_world);
    Ref<GaussianSplatWorld> get_world() const { return world; }

    void set_auto_apply_on_ready(bool p_enabled);
    bool is_auto_apply_on_ready() const { return auto_apply_on_ready; }

    void set_cast_shadow(bool p_enabled);
    bool is_cast_shadow() const { return cast_shadow; }

    void set_lod_enabled(bool p_enabled);
    bool is_lod_enabled() const { return lod_enabled; }

    void set_lod_bias(float p_bias);
    float get_lod_bias() const { return lod_bias; }

    void set_max_render_distance(float p_distance);
    float get_max_render_distance() const { return max_render_distance; }

    void set_max_splat_count(int p_count);
    int get_max_splat_count() const { return max_splat_count; }

    void set_use_frustum_culling(bool p_enabled);
    bool is_frustum_culling_enabled() const { return use_frustum_culling; }

    void set_async_upload_enabled(bool p_enabled);
    bool is_async_upload_enabled() const { return async_upload_enabled; }

    void set_opacity(float p_opacity);
    float get_opacity() const { return opacity; }

    void apply_world();
    void clear_world();

    Ref<GaussianSplatRenderer> get_renderer() const { return renderer; }

#ifdef TESTS_ENABLED
    // See world_resource_resubmit_run_count above. Test-only.
    uint64_t get_world_resource_resubmit_run_count_for_testing() const {
        return world_resource_resubmit_run_count;
    }
#endif
};

#endif // GAUSSIAN_SPLAT_WORLD_3D_H
