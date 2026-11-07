#ifndef SCENE_DIRECTOR_DEFERRED_WORK_H
#define SCENE_DIRECTOR_DEFERRED_WORK_H

// The deferred renderer-contract work (#611, #610 S6) of GaussianSplatSceneDirector (#610). Only the code that works on it includes this header;
// gaussian_splat_scene_director.h carries the forward declaration.

#include "gaussian_splat_scene_director.h"
#include "gaussian_splat_world_submission_contract.h"

class GaussianSplatSceneDirector::DeferredRendererWork {
public:
    enum class Kind : uint8_t {
        APPLY,
        RESTORE,
        INITIALIZE,
    };

private:
    struct Entry {
        Ref<GaussianSplatRenderer> renderer;
        GaussianSplatWorldSubmissionContract contract;
        GaussianSplatWorldSubmissionRuntimeStateSnapshot restore_state;
        Kind kind = Kind::APPLY;
    };
    LocalVector<Entry> entries;
    // Lifetime total of entries this queue actually dispatched. The only
    // observable that separates "flush ran the work" from "cancel dropped
    // it" without a live RenderingDevice, which no headless lane has.
    //
    // An INITIALIZE entry whose guard re-check finds the renderer already
    // initialized does NOT count: it was queued but not dispatched, and
    // conflating the two would make this counter unable to show that the
    // re-check fires.
    uint32_t dispatched_entry_count = 0;

public:
    void queue_apply(const Ref<GaussianSplatRenderer> &p_renderer,
            const GaussianSplatWorldSubmissionContract &p_contract);
    void queue_restore(const Ref<GaussianSplatRenderer> &p_renderer,
            const GaussianSplatWorldSubmissionRuntimeStateSnapshot &p_snapshot);
    // #611 PR B2: a restore that must run BEFORE anything already queued.
    //
    // This exists for exactly one caller: `submit_world_submission`'s
    // rollback, and it is a behavioural requirement rather than a
    // preference.
    //
    // When phase 1 lazily creates a renderer for a world that already has an
    // active record P, `_get_or_create_world_for_scenario` queues an apply of
    // P, and `target_previous_renderer_state` is snapshotted from the
    // still-blank renderer BEFORE that apply runs. The pre-#611 code then
    // restored INLINE and let the queued apply(P) flush afterwards, so P was
    // the last writer and the renderer ended up holding P's contract --
    // matching the director's record, which a rejection leaves at P.
    //
    // Appending the rollback instead inverts that: the flush order becomes
    // (apply(P), restore(blank)) and the blank snapshot wins, leaving the
    // renderer cleared while the director still considers P active. That is a
    // real divergence, not a cosmetic one, and it is the third ordering
    // defect this restructure has produced -- the queue and an inline call
    // racing to be last writer.
    //
    // Front-insertion restores the original ordering in every case: with a
    // queued apply present the sequence is (restore, apply(P)) as before, and
    // with an empty queue it degenerates to the plain append.
    //
    // NOT safe to front-insert ahead of an INITIALIZE entry (GPU bring-up must
    // precede any contract work). That cannot arise here: INITIALIZE is queued
    // only by `_initialize_world_renderer`, whose sole caller is
    // `register_instance`, never `submit_world_submission`. If that ever
    // changes, this must insert after the leading INITIALIZE entries instead.
    void queue_restore_first(const Ref<GaussianSplatRenderer> &p_renderer,
            const GaussianSplatWorldSubmissionRuntimeStateSnapshot &p_snapshot);
    // #611 PR B1: defer `GaussianSplatRenderer::initialize()`.
    //
    // ORDERING — this inserts at the FRONT of the queue, and that is
    // load-bearing. The one caller (`_initialize_world_renderer`, from
    // `register_instance`) decides to initialize *after*
    // `_get_or_create_world_for_scenario` may already have queued an apply
    // for the same lazily-created renderer, but the code this replaces ran
    // `initialize()` inline under the lock — i.e. BEFORE that queued apply
    // dispatched. Appending would invert that order and run the apply
    // against a renderer whose GPU resources are not up yet.
    //
    // The guard (`!gpu_resources_initialized && !gpu_initialization_pending`)
    // is re-evaluated at flush time, not captured here. Under the lock the
    // check and the call were atomic; deferring separates them, so the check
    // has to move with the call. It is never weaker than the inline form: the
    // only way the re-check can skip is if the renderer became initialized or
    // had an initialization queued in the gap, which is exactly what the
    // guard exists to detect.
    void queue_initialize(const Ref<GaussianSplatRenderer> &p_renderer);
    // Drop queued work that a later decision has superseded. Used where the
    // caller re-applies a newer contract to the same renderer before
    // returning; running the stale entry afterwards would clobber it.
    void cancel();
    bool is_empty() const { return entries.is_empty(); }
    uint32_t size() const { return entries.size(); }
    uint32_t get_dispatched_entry_count() const { return dispatched_entry_count; }
    // Dispatch-order inspection. Exists so the front-insertion rule above is
    // pinned by an assertion rather than by a comment: no headless lane can
    // observe entry order through side effects, because a device-less
    // renderer's initialize() and apply() converge on the same resource
    // state either way.
    Kind get_entry_kind(uint32_t p_index) const;
    // Runs and clears the queue. Must not be called while world_mutex is held.
    void flush();
    ~DeferredRendererWork() { flush(); }

    DeferredRendererWork() = default;
    DeferredRendererWork(const DeferredRendererWork &) = delete;
    DeferredRendererWork &operator=(const DeferredRendererWork &) = delete;
};

// #610 S6: RendererContractWorkQueue bundles the two deferred-renderer
// mechanisms every world_mutex critical section in this director must carry
// into ONE owned local, so the fragile relative destruction order they require
// stops being a per-call-site convention — a `deferred_renderer_release`
// vector hand-declared before a DeferredRendererWork, before the lock — and
// becomes an invariant of this type:
//
//   * a DeferredRendererWork queue — apply/restore/initialize work whose
//     blocking render-thread dispatch must run AFTER world_mutex is released
//     (#611); and
//   * the deferred-RELEASE vector — renderer Refs a prune
//     (_prune_world_if_unused) moved out of the `worlds` map under the lock,
//     whose ~GaussianSplatRenderer must likewise run unlocked or it is the
//     #628 indefinite editor hang.
//
// ORDER — LOAD-BEARING. The queued work must flush BEFORE the released
// renderer Refs drop: that is the order the pre-S6 inline code had (restore,
// then free), and release_world_submission still depends on it — it queues a
// restore against the very Ref the prune moved into release_vector(). Members
// are destroyed in reverse declaration order, so `deferred_release` is declared
// FIRST (destroyed LAST) and `work` LAST (destroyed FIRST, running flush() in
// ~DeferredRendererWork). No explicit destructor is needed: ~DeferredRendererWork
// flushes, then the vector drops.
//
// Declare a RendererContractWorkQueue local BEFORE the
// `ThreadOwnedMutexLock lock(world_mutex)` in every caller, exactly as its two
// constituents required individually; the whole object is then destroyed after
// the lock is released, so both the flush and the release happen unlocked.
// tests/ci/check_renderer_contract_boundary.py enforces this declaration order
// for RendererContractWorkQueue just as it does for a bare release vector.
class GaussianSplatSceneDirector::RendererContractWorkQueue {
public:
    // --- deferred contract-work primitives (forwarded to the queue) ---
    void queue_apply(const Ref<GaussianSplatRenderer> &p_renderer,
            const GaussianSplatWorldSubmissionContract &p_contract) {
        work.queue_apply(p_renderer, p_contract);
    }
    void queue_restore(const Ref<GaussianSplatRenderer> &p_renderer,
            const GaussianSplatWorldSubmissionRuntimeStateSnapshot &p_snapshot) {
        work.queue_restore(p_renderer, p_snapshot);
    }
    void queue_restore_first(const Ref<GaussianSplatRenderer> &p_renderer,
            const GaussianSplatWorldSubmissionRuntimeStateSnapshot &p_snapshot) {
        work.queue_restore_first(p_renderer, p_snapshot);
    }
    void queue_initialize(const Ref<GaussianSplatRenderer> &p_renderer) {
        work.queue_initialize(p_renderer);
    }
    void cancel() { work.cancel(); }
    // Runs and clears the queued contract work. Must not be called while
    // world_mutex is held (it dispatches). The deferred-release Refs are NOT
    // touched here; they drop with this object, after this flush.
    void flush() { work.flush(); }

    // --- deferred renderer-RELEASE vector (the #628 prune pattern) ---
    // The vector _prune_world_if_unused moves freed renderer Refs into under
    // world_mutex; they drop only when this object is destroyed — after the
    // lock is released and after the queued work above has flushed.
    LocalVector<Ref<GaussianSplatRenderer>> &release_vector() { return deferred_release; }

    RendererContractWorkQueue() = default;
    RendererContractWorkQueue(const RendererContractWorkQueue &) = delete;
    RendererContractWorkQueue &operator=(const RendererContractWorkQueue &) = delete;

private:
    // Declared FIRST → destroyed LAST: the freed renderer Refs drop only after
    // `work` (below) has flushed in its own destructor. See ORDER above.
    LocalVector<Ref<GaussianSplatRenderer>> deferred_release;
    // Declared LAST → destroyed FIRST: ~DeferredRendererWork runs flush().
    DeferredRendererWork work;
};

#endif // SCENE_DIRECTOR_DEFERRED_WORK_H
