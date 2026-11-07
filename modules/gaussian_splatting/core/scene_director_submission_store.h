#ifndef SCENE_DIRECTOR_SUBMISSION_STORE_H
#define SCENE_DIRECTOR_SUBMISSION_STORE_H

// SubmissionStore (S5) of GaussianSplatSceneDirector (#610). Only the code that works on it includes this header;
// gaussian_splat_scene_director.h carries the forward declaration.

#include "gaussian_splat_scene_director.h"
#include "gaussian_splat_world_submission_contract.h"

struct GaussianSplatSceneDirector::WorldSubmissionRecord {
Ref<GaussianSplatHlodRuntimeData> hlod_snapshot;
String hlod_reason;
float hlod_tau_px = 0.0f;
uint64_t hlod_world_key = 0;
    ObjectID owner_id;
    Ref<GaussianData> gaussian_data;
    Ref<ChunkPayloadSource> payload_source;
    Vector<StaticChunk> static_chunks;
    AABB bounds;
    Dictionary metadata;
    bool has_desired_residency_hint = false;
    int32_t desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
    Dictionary desired_renderer_overrides;
    bool casts_shadow = true;
    Callable hlod_demotion_callback;
    GaussianSplatWorldSubmissionRuntimeStateSnapshot renderer_restore_state;
    bool active = false;
};

// #610 S5: SubmissionStore owns the world-submission record — the active
// contract data a GaussianSplatWorld3D installs on a SharedWorld — together
// with the PURE functions that build a contract from it. It is DATA + pure
// construction ONLY.
//
// The three-phase submit protocol STAYS in the director: submit_world_submission
// arbitrates under world_mutex, applies the blocking render-thread dispatch with
// the lock RELEASED, then commits/re-validates under world_mutex, the whole
// sequence serialized by world_submission_apply_mutex. That protocol, its
// rollback ordering (queue_restore_first), its R1–R4 re-validation and the prune
// retry are orchestration, not record state, so they are deliberately NOT here —
// the store never dispatches, never locks and never decides.
//
// Like S3's InstanceStore it takes NO lock of its own: every access happens under
// the director's single world_mutex, preserving the one ThreadOwnedMutex
// ownership record the renderer-contract-boundary guard depends on.
//
// build_contract() is the pure contract construction: (renderer-state snapshot,
// record) -> WorldSubmissionContract, with zero side effects. It is defined in
// the .cpp so it keeps reaching the file-local dictionary/override helpers.
class GaussianSplatSceneDirector::SubmissionStore {
public:
    using WorldSubmissionRecord = GaussianSplatSceneDirector::WorldSubmissionRecord;

    // --- record queries ---
    bool is_active() const { return record.active; }
    ObjectID owner_id() const { return record.owner_id; }

    // Read-only view for the protocol's re-validation / getter reads and for the
    // pure helpers below. mutable_record() is for the protocol's in-place field
    // writes (renderer_restore_state) and the wholesale commit assignment; the
    // active-flag transitions those produce are byte-identical to the pre-S5
    // direct-field writes.
    const WorldSubmissionRecord &get_record() const { return record; }
    WorldSubmissionRecord &mutable_record() { return record; }

    // Clear the record back to the inactive default. Matches the prior
    // `world_submission = SharedWorld::WorldSubmissionRecord()` idiom used by
    // release, cross-scenario eviction and teardown.
    void reset() { record = WorldSubmissionRecord(); }

    // --- pure construction (no side effects, no dispatch, no lock) ---
    // Populate r_record from a submission DTO (owner/data/payload/overrides…),
    // resetting renderer_restore_state and marking the record active.
    static void store_submission(WorldSubmissionRecord &r_record, const WorldSubmission &p_submission);
    // True when the record carries resident or file-backed splats to draw.
    static bool record_has_renderable_payload(const WorldSubmissionRecord &p_record);
    // Build the renderer contract from a runtime-state baseline and the record.
    static GaussianSplatWorldSubmissionContract build_contract(
            const GaussianSplatWorldSubmissionRuntimeStateSnapshot &p_renderer_state,
            const WorldSubmissionRecord &p_record);

private:
    WorldSubmissionRecord record;
};

#endif // SCENE_DIRECTOR_SUBMISSION_STORE_H
