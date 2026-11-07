#ifndef SCENE_DIRECTOR_INSTANCE_STORE_H
#define SCENE_DIRECTOR_INSTANCE_STORE_H

// InstanceStore (S3) of GaussianSplatSceneDirector (#610). Only the code that works on it includes this header;
// gaussian_splat_scene_director.h carries the forward declaration.

#include "gaussian_splat_scene_director.h"
#include "../resources/color_grading_resource.h"

struct GaussianSplatSceneDirector::InstanceRecord {
    ObjectID node_id;
    Transform3D transform;
    float opacity = 1.0f;
    float lod_bias = 0.0f;
    float wind_intensity = 1.0f;
    uint32_t wind_mode = INSTANCE_WIND_INHERIT;
	Vector3 wind_direction = Vector3();
	float wind_frequency = 1.0f;
	float effect_position_scale = 1.0f;
	float effect_opacity_scale = 1.0f;
	// Full 64-bit ObjectID of the asset Node3D. Must NOT be truncated to
	// 32 bits: two assets whose ObjectIDs collide in the low 32 bits would
	// otherwise alias the same InstanceStore::asset_records entry.
	uint64_t asset_id = 0;
	uint32_t flags = 0;
    uint32_t last_lod = 0;
    bool casts_shadow = false;
    bool visible = true;
    bool has_desired_residency_hint = false;
    int32_t desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
    bool dirty = true;
    Ref<ColorGradingResource> color_grading;

    // Scene-effector filter state — cached on registration / setters so the
    // render thread never has to read these from the live Node3D.
    bool scene_effectors_enabled = true;
    uint32_t scene_effector_layer_mask = 1u;
    bool scene_effector_scope_filter_present = false;
    bool scene_effector_scope_filter_valid = true;
    ObjectID scene_effector_scope_root_id;
    LocalVector<ObjectID> scene_tree_ancestor_ids;
};

// Retained-asset payload keyed by the FULL 64-bit asset ObjectID. Moved out
// of SharedWorld and into InstanceStore (below) as part of #610 S3, which
// groups every instance/asset-lifetime field behind one owned component.
struct GaussianSplatSceneDirector::AssetRecord {
    Ref<GaussianSplatAsset> asset;
    Ref<GaussianData> data;
    uint32_t refcount = 0;
    uint32_t edited_version = 0;
    // DATA-001: the asset's payload_version at the time `data` was built. A DYNAMIC
    // asset re-populated at runtime bumps payload_version (edited_version does not, in
    // exports), so this lets retain_asset/refresh_asset rebuild the cached data.
    uint32_t payload_version = 0;
};

// #610 S3: InstanceStore owns the per-scenario instance records, the
// node->slot lookup, the instance/asset generation counters and the
// asset-retention table. It is the boundary the prune predicate
// (_world_has_no_instances) and the sibling decomposition slices see:
// callers touch only these public methods, never the fields -- that is what
// lets S4/S5/S8 be extracted independently.
//
// It takes NO lock of its own: every method is called under the director's
// single world_mutex, preserving the one ThreadOwnedMutex ownership record
// the renderer-contract-boundary guard depends on.
//
// InstanceRecord::last_lod deliberately stays inside the record and is
// written in place by the render-thread LOD walk
// (update_instance_lods_for_renderer). That LOD cache belongs to a later
// slice (S9), so the store exposes mutable_records() for the walk and hands
// out const views everywhere else.
class GaussianSplatSceneDirector::InstanceStore {
public:
    // --- instance queries ---
    bool is_empty() const { return instances.is_empty(); }
    uint32_t instance_count() const { return instances.size(); }
    bool has_instance(ObjectID p_node_id) const { return instance_lookup.has(p_node_id); }
    uint64_t generation() const { return instance_generation; }
    uint64_t asset_generation() const { return instance_asset_generation; }

    const InstanceRecord *find(ObjectID p_node_id) const {
        const uint32_t *index_ptr = instance_lookup.getptr(p_node_id);
        if (!index_ptr || *index_ptr >= instances.size()) {
            return nullptr;
        }
        return &instances[*index_ptr];
    }
    InstanceRecord *find_mutable(ObjectID p_node_id) {
        uint32_t *index_ptr = instance_lookup.getptr(p_node_id);
        if (!index_ptr || *index_ptr >= instances.size()) {
            return nullptr;
        }
        return &instances[*index_ptr];
    }
    // Read-only view for the render-path builders.
    const LocalVector<InstanceRecord> &records() const { return instances; }
    // Mutable view for the S9-owned render-thread LOD walk, which edits
    // last_lod / dirty in place. Callers MUST NOT push or remove through
    // this handle -- membership changes go through append() / remove() so
    // instance_lookup stays consistent.
    LocalVector<InstanceRecord> &mutable_records() { return instances; }

    // --- instance mutations (keep instance_lookup consistent) ---
    void append(const InstanceRecord &p_record) {
        instance_lookup[p_record.node_id] = instances.size();
        instances.push_back(p_record);
    }
    // Swap-remove the record for p_node_id, fixing up the moved record's
    // lookup slot. Returns false (and leaves r_asset_id untouched) when the
    // node is absent; otherwise sets r_asset_id to the removed record's
    // asset_id so the caller can release the matching asset record.
    bool remove(ObjectID p_node_id, uint64_t &r_asset_id) {
        uint32_t *index_ptr = instance_lookup.getptr(p_node_id);
        if (!index_ptr || *index_ptr >= instances.size()) {
            return false;
        }
        const uint32_t index = *index_ptr;
        r_asset_id = instances[index].asset_id;
        const uint32_t last_index = instances.size() - 1;
        if (index != last_index) {
            instances[index] = instances[last_index];
            instance_lookup[instances[index].node_id] = index;
        }
        instances.remove_at(last_index);
        instance_lookup.erase(p_node_id);
        return true;
    }

    // Saturating generation bumps (skip 0, the "unset" sentinel). Defined in
    // the .cpp so they can reuse the file-local saturating-increment helper.
    void bump_generation();
    void bump_asset_generation();

    // --- asset retention (retain/refresh/release policy moved out of the
    // director; see the .cpp definitions) ---
    bool retain_asset(const Ref<GaussianSplatAsset> &p_asset, uint64_t p_asset_id);
    bool refresh_asset(const Ref<GaussianSplatAsset> &p_asset, uint64_t p_asset_id);
    void release_asset(uint64_t p_asset_id);

    // --- asset queries ---
    const AssetRecord *find_asset(uint64_t p_asset_id) const { return asset_records.getptr(p_asset_id); }
    bool has_asset(uint64_t p_asset_id) const { return asset_records.has(p_asset_id); }
    uint32_t asset_count() const { return asset_records.size(); }
    bool assets_empty() const { return asset_records.is_empty(); }
    const HashMap<uint64_t, AssetRecord> &assets() const { return asset_records; }

    // Drop every instance, lookup slot and asset record. Used by
    // teardown_world_for_scenario, which erases the whole world entry right
    // after; the generation counters are intentionally left untouched,
    // matching the prior inline teardown.
    void clear() {
        instances.clear();
        instance_lookup.clear();
        asset_records.clear();
    }

private:
    static bool _populate_gaussian_data_from_asset(const Ref<GaussianSplatAsset> &p_asset, Ref<GaussianData> &r_data);
    // #798 review round 4: drop a record's cached payload after a FAILED rebuild,
    // so a failed retain/refresh evicts the stale GaussianData instead of leaving
    // the render path drawing it. See the .cpp definition for the full rationale.
    void _evict_asset_data(AssetRecord &r_record);

    LocalVector<InstanceRecord> instances;
    HashMap<ObjectID, uint32_t> instance_lookup;
    uint64_t instance_generation = 1;
    uint64_t instance_asset_generation = 1;
    HashMap<uint64_t, AssetRecord> asset_records;
};

#endif // SCENE_DIRECTOR_INSTANCE_STORE_H
