#ifndef SCENE_DIRECTOR_SPHERE_EFFECTOR_STORE_H
#define SCENE_DIRECTOR_SPHERE_EFFECTOR_STORE_H

// SphereEffectorStore (S4) of GaussianSplatSceneDirector (#610). Only the code that works on it includes this header;
// gaussian_splat_scene_director.h carries the forward declaration.

#include "gaussian_splat_scene_director.h"

struct GaussianSplatSceneDirector::SphereEffectorRecord {
    ObjectID effector_id;
    Transform3D transform;
    float radius = 0.0f;
    float strength = 0.0f;
    float falloff = 2.0f;
    float frequency = 2.0f;
    float opacity_strength = 1.0f;
    float target_opacity = 0.0f;
    uint32_t layer_mask = 1u;
    uint32_t scope_mode = SPHERE_EFFECTOR_SCOPE_SUBTREE;
    ObjectID scope_root_id;
    int32_t priority = 0;
    uint64_t registration_serial = 0;
    uint32_t scope_specificity = 0u;
    // Cached liveness of scope_root_id. Starts true on register, flipped
    // false (and triggers a generation bump) by the payload builder when
    // `ObjectDB::get_instance(scope_root_id)` no longer resolves.
    bool scope_root_valid = true;
    bool enabled = true;
    bool affect_position = true;
    bool affect_opacity = false;
};

// #610 S4: SphereEffectorStore owns the per-scenario sphere-effector
// records, the effector->slot lookup, the generation counter and the
// monotonic registration serial. It is the boundary the prune predicate
// (_world_has_no_sphere_effectors) and the sibling decomposition slices
// see: callers touch only these public methods, never the fields -- that
// is what lets S4/S5/S8 be extracted independently. Mirrors InstanceStore
// (#610 S3).
//
// It takes NO lock of its own: every method is called under the director's
// single world_mutex, preserving the one ThreadOwnedMutex ownership record
// the renderer-contract-boundary guard depends on.
//
// D5 (OPEN owner decision, NOT resolved by this slice): the render-thread
// payload builder (_build_sorted_sphere_effector_payload) revalidates each
// record's cached scope_root_valid against ObjectDB and, when it flips,
// writes the flag back through a const view (const_cast) and bumps the
// generation. That write-through-const behavior is preserved verbatim here;
// S4 is a pure move. records() hands out a const view and the builder keeps
// its existing const_cast to mutate scope_root_valid in place.
class GaussianSplatSceneDirector::SphereEffectorStore {
public:
    // --- queries ---
    bool is_empty() const { return sphere_effectors.is_empty(); }
    uint32_t count() const { return sphere_effectors.size(); }
    bool has_effector(ObjectID p_effector_id) const { return sphere_effector_lookup.has(p_effector_id); }
    uint64_t generation() const { return sphere_effector_generation; }

    // Read-only view for the render-thread payload builder and the
    // main-thread debug-state query. The builder mutates each record's
    // cached scope_root_valid in place via const_cast (D5, above);
    // membership never changes through this handle -- adds/removes go
    // through append()/remove() so sphere_effector_lookup stays consistent.
    const LocalVector<SphereEffectorRecord> &records() const { return sphere_effectors; }

    // Mutable lookup of a single record for the in-place update path.
    // Returns nullptr when the effector is absent.
    SphereEffectorRecord *find_mutable(ObjectID p_effector_id) {
        uint32_t *index_ptr = sphere_effector_lookup.getptr(p_effector_id);
        if (!index_ptr || *index_ptr >= sphere_effectors.size()) {
            return nullptr;
        }
        return &sphere_effectors[*index_ptr];
    }

    // --- mutations (keep sphere_effector_lookup consistent) ---
    // Stamp the record with the next registration serial, record its lookup
    // slot and append it. Mirrors the inline register path verbatim: the
    // serial is assigned BEFORE the copy is stored.
    void append(SphereEffectorRecord &p_record) {
        p_record.registration_serial = ++sphere_effector_registration_serial;
        sphere_effector_lookup[p_record.effector_id] = sphere_effectors.size();
        sphere_effectors.push_back(p_record);
    }
    // Swap-remove the record for p_effector_id, fixing up the moved
    // record's lookup slot. Returns false when the effector is absent
    // (leaving the store untouched), matching the prior inline guard.
    bool remove(ObjectID p_effector_id) {
        uint32_t *index_ptr = sphere_effector_lookup.getptr(p_effector_id);
        if (!index_ptr || *index_ptr >= sphere_effectors.size()) {
            return false;
        }
        const uint32_t index = *index_ptr;
        const uint32_t last_index = sphere_effectors.size() - 1;
        if (index != last_index) {
            sphere_effectors[index] = sphere_effectors[last_index];
            sphere_effector_lookup[sphere_effectors[index].effector_id] = index;
        }
        sphere_effectors.remove_at(last_index);
        sphere_effector_lookup.erase(p_effector_id);
        return true;
    }

    // Saturating generation bump (skip 0, the "unset"/"no world" sentinel
    // that get_sphere_effector_generation_for_renderer returns for a
    // missing world). Defined in the .cpp so it reuses the file-local
    // saturating-increment helper. #610 S4 (D2): the register/update/remove
    // paths previously bumped through that saturating helper while the two
    // render-thread D5 revalidation sites used a raw `++` (wrap-to-0 on
    // uint64 overflow). All six now route through this one saturating
    // method, so the two D5 sites wrap to 1 instead of 0. Because 0 is the
    // reserved sentinel above, that is a deliberate consistency fix that
    // closes a spurious-0-on-overflow hazard on those two sites -- NOT a
    // 1:1 move -- and it is unreachable in practice (2^64 bumps). Firing
    // conditions and order are otherwise unchanged. See the D2 note at the
    // two sites in scene_director_sphere_effectors.cpp.
    void bump_generation();

    // Drop every effector and lookup slot. Used by
    // teardown_world_for_scenario, which erases the whole world entry right
    // after; the generation/serial counters are intentionally left
    // untouched, matching the prior inline teardown.
    void clear() {
        sphere_effectors.clear();
        sphere_effector_lookup.clear();
    }

private:
    LocalVector<SphereEffectorRecord> sphere_effectors;
    HashMap<ObjectID, uint32_t> sphere_effector_lookup;
    uint64_t sphere_effector_generation = 1;
    uint64_t sphere_effector_registration_serial = 0;
};

#endif // SCENE_DIRECTOR_SPHERE_EFFECTOR_STORE_H
