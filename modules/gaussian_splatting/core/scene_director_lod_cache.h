#ifndef SCENE_DIRECTOR_LOD_CACHE_H
#define SCENE_DIRECTOR_LOD_CACHE_H

// LODCacheOwner (S9) of GaussianSplatSceneDirector (#610). Only the code that works on it includes this header;
// gaussian_splat_scene_director.h carries the forward declaration.

#include "gaussian_splat_scene_director.h"
#include "scene_director_instance_store.h"

// #610 S9: LODCacheOwner owns the per-scenario LOD-walk memoization AND the
// O(instances) LOD walk itself (the body of update_instance_lods_for_renderer).
// The five memo fields used to live inline in SharedWorld and were touched
// nowhere but that walk (early-out + write-back), so they encapsulate cleanly
// here -- the same store-extraction pattern as S3/S4/S5/S7.
//
// Like every sibling store it takes NO lock of its own: update() is called
// ONLY from update_instance_lods_for_renderer, on the RENDER THREAD, under the
// director's single world_mutex -- preserving the one ThreadOwnedMutex
// ownership record the renderer-contract-boundary guard depends on. Do NOT give
// it a mutex; that would fragment that ownership record.
//
// The render-thread carve-out (already documented on InstanceStore): update()
// mutates p_store.mutable_records()[i].last_lod / .dirty in place and bumps the
// store's generation on change. This is the one place the render thread writes
// back into director-owned records; it is safe because world_mutex fully
// serializes it against every main-thread instance mutation. Whether to keep
// this render-thread write or relocate it (main-thread writer / render-owned
// double-buffer) is the H1 decision, deferred to #749 as a separate behavior-
// changing PR. S9 is a behavior-preserving extraction: no visual or timing
// change relative to the pre-S9 inline walk.
class GaussianSplatSceneDirector::LODCacheOwner {
public:
    // Runs the memoized O(instances) LOD walk. Called only under world_mutex.
    // Takes no lock. Mutates last_lod / dirty in p_store's records in place and
    // bumps p_store's generation when any instance's LOD changed.
    void update(InstanceStore &p_store, const Vector3 &p_camera_pos,
            const LODConfig &p_lod_config, float p_hysteresis_zone);

    // Test / diagnostics only.
    bool cache_valid() const { return lod_walk_cache_valid; }
    uint64_t last_generation() const { return lod_walk_last_generation; }

private:
    // Per-instance LOD-walk memoization (host-side, zero visual effect). The
    // walk's result is a pure function of (camera position, every instance
    // transform/bias, the LODConfig, the hysteresis zone). The store's
    // generation already bumps on every instance add/remove/transform/param
    // change, so the whole walk is skipped when none of those inputs moved
    // since last time. Recorded AFTER the walk so the walk's own generation
    // bump is captured and does not force a redundant re-walk next frame.
    bool lod_walk_cache_valid = false;
    Vector3 lod_walk_last_camera_pos;
    uint64_t lod_walk_last_generation = 0;
    LODConfig lod_walk_last_config;
    float lod_walk_last_hysteresis = 0.0f;
};

#endif // SCENE_DIRECTOR_LOD_CACHE_H
