#ifndef SCENE_DIRECTOR_WORLD_REGISTRY_H
#define SCENE_DIRECTOR_WORLD_REGISTRY_H

// SharedWorld and the inline WorldRegistry methods of GaussianSplatSceneDirector (#610 S8). This is the
// one include for the director's .cpp files: it pulls in every component header.

#include "gaussian_splat_scene_director.h"
#include "scene_director_deferred_work.h"
#include "scene_director_instance_store.h"
#include "scene_director_lod_cache.h"
#include "scene_director_renderer_lifecycle.h"
#include "scene_director_sphere_effector_store.h"
#include "scene_director_submission_store.h"

struct GaussianSplatSceneDirector::SharedWorld {
    RID scenario;
    RendererLifecycleOwner renderer_owner;
    InstanceStore instance_store;
    SphereEffectorStore sphere_effector_store;
    SubmissionStore submission_store;
    // #610 S9: the per-instance LOD-walk memoization + the walk itself.
    LODCacheOwner lod_cache_owner;
};

inline bool GaussianSplatSceneDirector::WorldRegistry::is_empty() const { return worlds.is_empty(); }
inline uint32_t GaussianSplatSceneDirector::WorldRegistry::size() const { return worlds.size(); }
inline bool GaussianSplatSceneDirector::WorldRegistry::has(const RID &p_scenario) const { return worlds.has(p_scenario); }
inline GaussianSplatSceneDirector::SharedWorld * GaussianSplatSceneDirector::WorldRegistry::find(const RID &p_scenario) { return worlds.getptr(p_scenario); }
inline const GaussianSplatSceneDirector::SharedWorld * GaussianSplatSceneDirector::WorldRegistry::find(const RID &p_scenario) const { return worlds.getptr(p_scenario); }
inline GaussianSplatSceneDirector::SharedWorld * GaussianSplatSceneDirector::WorldRegistry::insert(const RID &p_scenario, const GaussianSplatSceneDirector::SharedWorld &p_world) {
    worlds.insert(p_scenario, p_world);
    return worlds.getptr(p_scenario);
}
inline void GaussianSplatSceneDirector::WorldRegistry::erase(const RID &p_scenario) { worlds.erase(p_scenario); }
inline void GaussianSplatSceneDirector::WorldRegistry::clear() { worlds.clear(); }
inline HashMap<RID, GaussianSplatSceneDirector::SharedWorld>::Iterator GaussianSplatSceneDirector::WorldRegistry::begin() { return worlds.begin(); }
inline HashMap<RID, GaussianSplatSceneDirector::SharedWorld>::Iterator GaussianSplatSceneDirector::WorldRegistry::end() { return worlds.end(); }
inline HashMap<RID, GaussianSplatSceneDirector::SharedWorld>::ConstIterator GaussianSplatSceneDirector::WorldRegistry::begin() const { return worlds.begin(); }
inline HashMap<RID, GaussianSplatSceneDirector::SharedWorld>::ConstIterator GaussianSplatSceneDirector::WorldRegistry::end() const { return worlds.end(); }

#endif // SCENE_DIRECTOR_WORLD_REGISTRY_H
