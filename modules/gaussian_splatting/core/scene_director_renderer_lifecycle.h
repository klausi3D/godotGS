#ifndef SCENE_DIRECTOR_RENDERER_LIFECYCLE_H
#define SCENE_DIRECTOR_RENDERER_LIFECYCLE_H

// RendererLifecycleOwner (S7) of GaussianSplatSceneDirector (#610). Only the code that works on it includes this header;
// gaussian_splat_scene_director.h carries the forward declaration.

#include "gaussian_splat_scene_director.h"

// #610 S7: RendererLifecycleOwner owns the per-scenario shared
// Ref<GaussianSplatRenderer> and the two lifecycle transitions that create
// and release it -- lazy instantiation (memnew on first demand) and the
// move-out that hands the last Ref to a caller's deferred-release path. It is
// the boundary the renderer-Ref lifecycle presents to the rest of the
// director: instance / sphere-effector / world-submission code sees only the
// renderer's IDENTITY (ptr() / is_valid() / is_null()) or an opaque strong Ref
// (ref()), never the owned field. Mirrors S3's InstanceStore / S4's
// SphereEffectorStore / S5's SubmissionStore.
//
// Like those stores it takes NO lock of its own: every access happens under
// the director's single world_mutex, preserving the one ThreadOwnedMutex
// ownership record the renderer-contract-boundary guard depends on. It adds no
// mutex and holds no back-pointer to the director.
//
// DELIBERATELY NOT MOVED HERE -- these stay on the director because they are
// the #628 / #611 renderer-contract territory this slice must not perturb, not
// renderer-Ref lifecycle:
//   * GPU BRING-UP. `_initialize_world_renderer` dispatches
//     GaussianSplatRenderer::initialize() across the blocking render-thread
//     boundary and is one of the functions
//     tests/ci/check_renderer_contract_boundary.py allowlists + instruments
//     (it must reach world_mutex ownership + _report_renderer_contract_lock_violation).
//     It stays on the director and reaches the renderer through ref().
//   * The three-phase submit protocol's PHASE-2 HELD REF. submit_world_submission
//     copies the strong Ref (ref()) to inflate the reference count to 2 across
//     the unlocked apply, then unref()s it and retries the prune (#611 PR B2).
//     That protocol -- the copy, the unref, the retry -- stays verbatim in the
//     director; this owner only vends the Ref it copies.
//   * The DEFERRED LAST-REF RELEASE ordering (#628). A pruned / torn-down
//     renderer's Ref is move()d out via release() into a caller-owned release
//     vector or a local declared BEFORE world_mutex, so ~GaussianSplatRenderer
//     (a p_allow_timeout=false blocking dispatch) runs only after the lock is
//     released. This owner never drops the last Ref itself; release() just
//     transfers ownership to the caller, whose declaration order the guard's
//     check_renderer_ref_released_under_lock still enforces.
class GaussianSplatSceneDirector::RendererLifecycleOwner {
public:
    // --- renderer identity / state (what instance/sphere/submission code sees) ---
    bool is_valid() const { return renderer.is_valid(); }
    bool is_null() const { return renderer.is_null(); }
    GaussianSplatRenderer *ptr() const { return renderer.ptr(); }
    // Opaque strong Ref. Callers that must copy it (the phase-2 protocol, the
    // deferred-work queue's queue_apply/queue_restore, get_shared_renderer, the
    // InstanceSubmission DTO) or deref it for a NON-dispatching read (snapshot /
    // get_resource_state) go through here; the identity and refcount semantics
    // are exactly those of the former SharedWorld::renderer field.
    const Ref<GaussianSplatRenderer> &ref() const { return renderer; }
    // Renderer reference count, or 0 when null. Exists solely for the prune
    // predicate `_world_renderer_unshared`, which preserves its historical
    // `is_null() || get_reference_count() <= 1` threshold by reading through
    // this (the is_null() short-circuit still guards the deref).
    int reference_count() const;

    // --- lifecycle: create + release (the ONLY two field mutations) ---
    // Lazily instantiate the shared renderer for this world's device.
    // Precondition (unchanged from the inline site): the caller already
    // established the renderer is absent. A pure move of the former inline
    // `renderer = Ref<GaussianSplatRenderer>(memnew(GaussianSplatRenderer(device)))`.
    void create(RenderingDevice *p_device);
    // Move the strong Ref out for deferred, UNLOCKED release, leaving the owner
    // empty. Mirrors the former `std::move(world->renderer)`. The caller MUST
    // let the returned Ref drop only after world_mutex is released (it is pushed
    // into a release vector / assigned to a local declared before the lock) --
    // dropping the last Ref under the lock is the #628 indefinite hang.
    Ref<GaussianSplatRenderer> release();

private:
    Ref<GaussianSplatRenderer> renderer;
};

#endif // SCENE_DIRECTOR_RENDERER_LIFECYCLE_H
