#ifndef GAUSSIAN_SPLAT_SCENE_DIRECTOR_H
#define GAUSSIAN_SPLAT_SCENE_DIRECTOR_H

#include "core/object/object.h"
#include "core/object/object_id.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/math/transform_3d.h"
#include "core/math/aabb.h"
#include "core/variant/variant.h"
#include "scene/resources/3d/world_3d.h"

#include "gaussian_data.h"
#include "gaussian_splat_asset.h"
#include "streaming_chunk_payload_source.h"
#include "thread_owned_mutex.h"
#include "../lod/lod_config.h"
#include "../renderer/gaussian_splat_renderer.h"

class ColorGradingResource;
class Node;
class Node3D;
class RenderingDevice;

class GaussianSplatSceneDirector : public Object {
    GDCLASS(GaussianSplatSceneDirector, Object);

public:
    enum InstanceWindMode : uint32_t {
        INSTANCE_WIND_INHERIT = 0u,
        INSTANCE_WIND_FORCE_DISABLED = 1u,
        INSTANCE_WIND_FORCE_ENABLED = 2u,
    };

    enum SubmissionResidencyHint : int32_t {
        SUBMISSION_RESIDENCY_HINT_RESIDENT = 0,
        SUBMISSION_RESIDENCY_HINT_STREAMING = 1,
    };

    struct InstanceSubmission {
        ObjectID node_id;
        RID scenario;
        Ref<GaussianSplatRenderer> renderer;
        Ref<GaussianSplatAsset> asset;
        Transform3D transform;
        float opacity = 1.0f;
        float lod_bias = 0.0f;
        float wind_intensity = 1.0f;
        uint32_t wind_mode = INSTANCE_WIND_INHERIT;
        Vector3 wind_direction = Vector3();
        float wind_frequency = 1.0f;
        float effect_position_scale = 1.0f;
        float effect_opacity_scale = 1.0f;
        uint32_t flags = 0;
        uint32_t last_lod = 0;
        bool casts_shadow = false;
        bool visible = true;
        bool has_desired_residency_hint = false;
        int32_t desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
        Ref<ColorGradingResource> color_grading;
    };

    struct WorldSubmission {
        ObjectID owner_id;
        RID scenario;
        Ref<GaussianData> gaussian_data;
        Ref<ChunkPayloadSource> payload_source;
        Vector<GaussianSplatRenderer::StaticChunk> static_chunks;
        AABB bounds;
        Dictionary metadata;
        bool has_desired_residency_hint = false;
        int32_t desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
        Dictionary desired_renderer_overrides;
    };

    struct SubmissionCounts {
        uint32_t instance_submissions = 0;
        uint32_t world_submissions = 0;
    };

    enum SphereEffectorScopeMode : uint32_t {
        SPHERE_EFFECTOR_SCOPE_WORLD = 0u,
        SPHERE_EFFECTOR_SCOPE_SUBTREE = 1u,
        SPHERE_EFFECTOR_SCOPE_EXPLICIT_ROOT = 2u,
    };

    struct SphereEffectorSelection {
        ObjectID effector_id;
        RID scenario;
        Transform3D transform;
        Vector3 center;
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
        uint32_t matched_effector_count = 0;
        bool enabled = false;
        bool affect_position = true;
        bool affect_opacity = false;
    };

    static GaussianSplatSceneDirector *get_singleton();

    GaussianSplatSceneDirector();
    ~GaussianSplatSceneDirector();

	void register_instance(ObjectID p_node_id, const Ref<GaussianSplatAsset> &p_asset, const Transform3D &p_transform,
			float p_opacity, float p_lod_bias, uint32_t p_flags, bool p_casts_shadow = false,
			float p_wind_intensity = 1.0f, uint32_t p_wind_mode = INSTANCE_WIND_INHERIT,
			const Vector3 &p_wind_direction = Vector3(), float p_wind_frequency = 1.0f,
			bool p_visible = true, bool p_has_desired_residency_hint = false,
			int32_t p_desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT,
			float p_effect_position_scale = 1.0f, float p_effect_opacity_scale = 1.0f);
	void update_instance_transform(ObjectID p_node_id, const Transform3D &p_transform);
	// Cache the per-instance scene-effector filter state on the InstanceRecord. Called
	// from GaussianSplatNode3D setters so the render thread never has to read these
	// fields back from the live Node3D during build_instance_buffer_for_renderer.
	void update_instance_scene_effector_filter(ObjectID p_node_id, bool p_enabled,
			uint32_t p_layer_mask, bool p_scope_filter_present, bool p_scope_filter_valid,
			ObjectID p_scope_root_id, const LocalVector<ObjectID> &p_scene_tree_ancestor_ids);
	void update_instance_params(ObjectID p_node_id, float p_opacity, float p_lod_bias, uint32_t p_flags, bool p_casts_shadow = false,
			float p_wind_intensity = 1.0f, uint32_t p_wind_mode = INSTANCE_WIND_INHERIT,
			const Vector3 &p_wind_direction = Vector3(), float p_wind_frequency = 1.0f,
			bool p_visible = true, bool p_has_desired_residency_hint = false,
			int32_t p_desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT,
			float p_effect_position_scale = 1.0f, float p_effect_opacity_scale = 1.0f);
	void unregister_instance(ObjectID p_node_id);
    void update_instance_lods_for_renderer(const GaussianSplatRenderer *p_renderer, const Vector3 &p_camera_pos,
            const LODConfig &p_lod_config, float p_hysteresis_zone);
    void build_instance_buffer(LocalVector<InstanceDataGPU> &out) const;
	// Builds the per-instance GPU rows. When r_submission_asset_ids is non-null it is
	// filled parallel to `out` with each instance's FULL 64-bit submission asset
	// identity (the asset_records key). The GPU InstanceDataGPU::ids[0] field is only
	// 32 bits and therefore cannot carry that identity losslessly once 64-bit ObjectIDs
	// are in play; update_instance_buffer() uses this parallel array as the collision-
	// free key into PublishedInstanceAssetRemap before overwriting ids[0] with the
	// resolved dense slot.
	void build_instance_buffer_for_renderer(const GaussianSplatRenderer *p_renderer, LocalVector<InstanceDataGPU> &out,
			bool p_shadow_casters_only = false,
			LocalVector<uint64_t> *r_submission_asset_ids = nullptr) const;
	// Build the per-instance color grading SSBO for the supplied renderer. Walks the same
	// instance list as build_instance_buffer_for_renderer; falls back to the renderer's
	// RenderConfig::color_grading when a record has no per-instance ref. When no director
	// instances exist but the renderer is active (early setup / legacy world-submission shim),
	// produces a 1-row buffer so the shader always has a valid index.
	void build_instance_grading_buffer_for_renderer(const GaussianSplatRenderer *p_renderer,
			LocalVector<InstanceGradingGPU> &out, bool p_shadow_casters_only = false) const;
	// Fill a single GPU grading row from a ColorGradingResource ref (null → neutral
	// disabled). Exposed so streaming/resident fallback paths that inject synthetic
	// instance rows outside the director's record list can still honor the renderer's
	// color_grading default instead of forcing neutral.
	static void fill_instance_grading_entry(const Ref<ColorGradingResource> &p_grading,
			InstanceGradingGPU &r_entry);
	// Per-instance color grading setter. Stores the grading ref on the record identified
	// by node_id; the next frame's build_instance_grading_buffer_for_renderer picks it up.
	// No-op when the node is unregistered.
	//
	// `p_force_refresh` controls cache-invalidation cadence. Callers that know the
	// underlying grading values just changed (e.g. a ColorGradingResource `changed`
	// signal for slider edits) pass true — the generation is bumped even when the
	// ref is unchanged so the buffer re-uploads with fresh values. Callers that
	// merely echo the current ref (per-frame apply) leave it false so unrelated
	// setting churn does not bust sort/raster caches every frame.
	bool update_instance_color_grading(ObjectID p_node_id, const Ref<ColorGradingResource> &p_grading,
			bool p_force_refresh = false);
	// Accessor for tests and diagnostics.
	Ref<ColorGradingResource> get_instance_color_grading(ObjectID p_node_id) const;
	// Bump the instance generation of the world bound to this renderer so the
	// next frame rebuilds the grading SSBO. Called when the renderer's legacy
	// renderer-wide color_grading default changes — records with no per-instance
	// grading read from that default via the build step's fallback, so their
	// rows need to re-upload even though no per-instance ref changed.
	void invalidate_grading_for_renderer(const GaussianSplatRenderer *p_renderer);
	// Hash every per-instance grading bound to this renderer. Used by the sort/raster cache
	// invalidation path so any node's grading edit busts the cache.
	//
	// `p_shadow_casters_only` mirrors the filter in build_instance_grading_buffer_for_renderer.
	// When the renderer is rendering a shadow pass, non-shadow-caster records are filtered
	// out of the grading buffer — their gradings MUST not participate in the shadow cache
	// signature either, otherwise grading edits on non-shadow nodes spuriously bust the
	// shadow sort/raster cache.
	uint64_t compute_color_grading_signature_for_renderer(const GaussianSplatRenderer *p_renderer,
			bool p_shadow_casters_only = false) const;
	uint32_t get_instance_count_for_renderer(const GaussianSplatRenderer *p_renderer) const;
	// Node IDs of every instance currently registered against `p_renderer`.
	//
	// #329: the P2 "renderer is shared" gate is a function of this set, so a node
	// that joins or leaves has to be able to tell its PEERS to re-evaluate — the
	// peer's own state is stale the instant the set changes and nothing else on a
	// non-per-frame path re-reads it. Returns IDs (not pointers) so the caller
	// resolves through ObjectDB and cannot act on a freed node.
	//
	// Takes world_mutex, so it must NOT be called while the caller already holds
	// it (i.e. never from inside another director method under the lock).
	void collect_instance_node_ids_for_renderer(const GaussianSplatRenderer *p_renderer,
			LocalVector<ObjectID> &r_node_ids) const;
	uint64_t get_instance_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    uint64_t get_instance_asset_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    void register_sphere_effector(ObjectID p_effector_id, const Transform3D &p_transform,
            float p_radius, float p_strength, float p_falloff, float p_frequency,
            bool p_enabled = true, bool p_affect_position = true, bool p_affect_opacity = false,
            float p_opacity_strength = 1.0f, float p_target_opacity = 0.0f, uint32_t p_layer_mask = 1u,
            uint32_t p_scope_mode = SPHERE_EFFECTOR_SCOPE_SUBTREE,
            ObjectID p_scope_root_id = ObjectID(), int32_t p_priority = 0);
    void update_sphere_effector(ObjectID p_effector_id, const Transform3D &p_transform,
            float p_radius, float p_strength, float p_falloff, float p_frequency,
            bool p_enabled = true, bool p_affect_position = true, bool p_affect_opacity = false,
            float p_opacity_strength = 1.0f, float p_target_opacity = 0.0f, uint32_t p_layer_mask = 1u,
            uint32_t p_scope_mode = SPHERE_EFFECTOR_SCOPE_SUBTREE,
            ObjectID p_scope_root_id = ObjectID(), int32_t p_priority = 0);
    void unregister_sphere_effector(ObjectID p_effector_id);
    // Build the renderer's effector payload. When `r_total_scene_effectors` is
    // non-null, it also returns the raw effector count for this renderer's
    // world under the same `world_mutex` lock — callers that need both values
    // should use this overload to avoid a double-query race where the main
    // thread can mutate the effector list between two director calls.
    void build_sphere_effector_payload_for_renderer(const GaussianSplatRenderer *p_renderer,
            LocalVector<SphereEffectorSelection> &out,
            uint32_t *r_total_scene_effectors = nullptr) const;
    Dictionary get_scene_effector_debug_state_for_instance(ObjectID p_node_id) const;
    uint32_t get_sphere_effector_count_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    uint64_t get_sphere_effector_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    void register_instance_submission(ObjectID p_node_id, const Ref<GaussianSplatAsset> &p_asset,
            const Transform3D &p_transform, float p_opacity, float p_lod_bias, uint32_t p_flags,
            bool p_casts_shadow = false, float p_wind_intensity = 1.0f,
            uint32_t p_wind_mode = INSTANCE_WIND_INHERIT, const Vector3 &p_wind_direction = Vector3(),
            float p_wind_frequency = 1.0f, bool p_visible = true,
            bool p_has_desired_residency_hint = false,
            int32_t p_desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT,
            float p_effect_position_scale = 1.0f, float p_effect_opacity_scale = 1.0f);
    void update_instance_submission_transform(ObjectID p_node_id, const Transform3D &p_transform);
    void update_instance_submission_params(ObjectID p_node_id, float p_opacity, float p_lod_bias, uint32_t p_flags,
            bool p_casts_shadow = false, float p_wind_intensity = 1.0f,
            uint32_t p_wind_mode = INSTANCE_WIND_INHERIT, const Vector3 &p_wind_direction = Vector3(),
            float p_wind_frequency = 1.0f, bool p_visible = true,
            bool p_has_desired_residency_hint = false,
            int32_t p_desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT,
            float p_effect_position_scale = 1.0f, float p_effect_opacity_scale = 1.0f);
    void unregister_instance_submission(ObjectID p_node_id);
    bool get_instance_submission(ObjectID p_node_id, InstanceSubmission *r_submission) const;

	void collect_instance_assets_for_renderer(const GaussianSplatRenderer *p_renderer, LocalVector<InstanceAssetRegistration> &out,
			bool p_shadow_casters_only = false) const;
	// Like collect_instance_assets_for_renderer(), but returns every asset retained by this
	// renderer's shared world regardless of any instance's current visibility or shadow-casting
	// state. Used by the resident contract publisher so the resident atlas is a stable superset
	// of registered content -- visibility/casts_shadow flips never mutate atlas membership and
	// therefore never trigger a full atlas repack. Streaming and renderer-quality callers that
	// must react to per-frame visibility keep using collect_instance_assets_for_renderer().
	void collect_registered_assets_for_renderer(const GaussianSplatRenderer *p_renderer,
			LocalVector<InstanceAssetRegistration> &out) const;
    // Runtime world-submission path. Applies the submitted payload to the shared renderer and
    // becomes the authoritative active world-backed source for the scenario.
    bool submit_world_submission(const WorldSubmission &p_submission);
    // Runtime inverse of submit_world_submission(). Clears renderer-owned world state and
    // releases the active world-backed source for this owner.
    void release_world_submission(ObjectID p_owner_id);
    // Explicit, idempotent teardown of every SharedWorld entry bound to this scenario.
    //
    // Drops the director's owned Ref<GaussianSplatRenderer> and clears all GPU-resource-bearing
    // refs (asset records, world-submission record).
    //
    // NOT called from any node's NOTIFICATION_PREDELETE. Both GaussianSplatWorld3D
    // (gaussian_splat_world_3d.cpp) and GaussianSplatNode3D (gaussian_splat_node_3d.cpp)
    // deliberately use the ownership-aware release_world_submission() +
    // try_prune_world_if_unused() pair instead, and each carries a comment explaining why:
    // a scenario-wide teardown would wipe the SharedWorld (instances, world-submission,
    // renderer ref) shared by sibling nodes / a still-live peer world node in the same
    // scenario. Do not "restore" a PREDELETE call here.
    //
    // Live callers are:
    //   * release_all_worlds() (below), which reuses this path for its per-scenario teardown
    //     because it already implements the #611 deferred renderer-release discipline; and
    //   * tests (test_renderer_lifetime_proof.h, test_scene_director_submission_scaffolding.h).
    // The motivating scenario is still editor F6 reload -- which throws the SceneTree away
    // without invoking `~GaussianSplatSceneDirector` -- reached today through
    // release_all_worlds() rather than through a node notification.
    //
    // Bypasses the `_should_prune_world` refcount>1 guard intentionally: external Refs held
    // by the about-to-be-deleted scene tree nodes will drop in their own dtors that follow
    // PREDELETE. After teardown the next register_* call rebuilds the SharedWorld lazily.
    void teardown_world_for_scenario(const RID &p_scenario);
    // Idempotent teardown of EVERY SharedWorld, equivalent to what
    // `~GaussianSplatSceneDirector` does via world_registry.clear() but callable while the
    // engine is still fully alive. Added for the --gs-gpu-test harness (#329), which
    // must free renderer-owned GPU resources before it destroys the RenderingDevice
    // it owns, and destroy that device before Main::test_cleanup() deletes Engine.
    // Safe to call at any point: after it returns the director simply rebuilds each
    // SharedWorld lazily on the next register_* call.
    void release_all_worlds();
    // Public wrapper around _prune_world_if_unused. Required by per-instance PREDELETE
    // handlers (GaussianSplatNode3D and GaussianSplatWorld3D) to garbage-collect the
    // SharedWorld AFTER renderer.unref() finally drops the node's reference. The
    // earlier NOTIFICATION_EXIT_TREE prune call still observes refcount>1 because the
    // node still holds its renderer Ref at that point; the second unregister call in
    // PREDELETE is a no-op (the instance/world-submission record is already gone), so
    // it never reaches the internal prune helper with the reduced refcount. Without
    // this explicit call the SharedWorld lingers across F6 reload cycles holding the
    // renderer/data lifetime anchor. See Codex review comments #3294797692 and
    // #3294797697 on PR #387.
    void try_prune_world_if_unused(const RID &p_scenario);
    // Test/diagnostics-only: returns true iff a SharedWorld entry exists for the
    // given scenario in the director's map. Distinct from get_shared_renderer(),
    // which lazily creates the entry on a miss.
    bool has_shared_world_for_scenario(const RID &p_scenario) const;
    // #611: how many times a renderer-contract entry point
    // (_apply_world_submission_to_renderer / _restore_world_submission_renderer /
    // _initialize_world_renderer) reached a live renderer while the calling
    // thread already held world_mutex.
    //
    // READ THIS AS: "an ordering violation occurred", NOT "a stall occurred".
    //   * The count says the lock was held across the renderer-contract boundary.
    //     Whether the dispatch that follows actually blocks depends on the run —
    //     under `--headless` RenderThreadDispatcher short-circuits and returns
    //     immediately (render_thread_dispatcher.cpp:17-22 and :116-122), so a
    //     headless run can increment this without anything stalling.
    //   * SCOPE. Every route from this class to a blocking render-thread dispatch
    //     now passes through one of the three instrumented functions above:
    //     `apply_world_submission_contract` and
    //     `restore_world_submission_runtime_state` via the first two,
    //     `GaussianSplatRenderer::initialize` via the third. PR A's revision of
    //     this comment called the counter a LOWER BOUND because
    //     register_instance called `initialize()` directly, bypassing it; that
    //     call now goes through `_initialize_world_renderer`, so the gap is
    //     closed. This is a *structural* claim about the current call graph, not
    //     a guarantee: a new direct `renderer->` call that dispatches would be
    //     invisible again. The static guard in
    //     tests/ci/check_renderer_contract_boundary.py is what keeps it true.
    //
    // No lane in this repo can reproduce the stall behaviourally (every doctest
    // process runs `--headless --test`, tests/ci/run_module_tests.py:350), so an
    // inspectable counter is the honest substitute — provided it is read as the
    // ordering signal it is.
    //
    // Process-wide, monotonic except for the explicit reset below.
    static uint64_t get_renderer_contract_lock_violation_count();
    static void reset_renderer_contract_lock_violation_count();
#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)
    // Test/diagnostics-only: returns the SharedWorld::asset_records key the
    // director derives from an asset's ObjectID. Exposed so a regression test
    // can prove two ObjectIDs colliding in the low 32 bits do NOT alias.
    static uint64_t test_asset_records_key(ObjectID p_asset_object_id) {
        return _asset_records_key(p_asset_object_id);
    }
    // Test/diagnostics-only: number of distinct asset records retained in the
    // SharedWorld bound to the given scenario (0 if no world exists).
    uint32_t test_asset_record_count_for_scenario(const RID &p_scenario) const;
    // Test/diagnostics-only: true iff the SharedWorld for the scenario holds an
    // asset record under the full 64-bit ObjectID key.
    bool test_has_asset_record_for_scenario(const RID &p_scenario, ObjectID p_asset_object_id) const;
    // Test/diagnostics-only: the InstanceStore instance / asset generation
    // counters for the SharedWorld bound to the given scenario (0 if no world
    // exists). These read the EXACT same InstanceStore::generation() /
    // asset_generation() values that get_instance_generation_for_renderer() /
    // get_instance_asset_generation_for_renderer() return once a renderer is
    // attached -- the only difference is the world is located by scenario rather
    // than by renderer pointer. Exposed so the cache-invalidation bump can be
    // exercised in a headless [SceneDirector] doctest without a RenderingDevice.
    uint64_t test_instance_generation_for_scenario(const RID &p_scenario) const;
    uint64_t test_instance_asset_generation_for_scenario(const RID &p_scenario) const;
#endif
    bool get_world_submission(ObjectID p_owner_id, WorldSubmission *r_submission) const;
    bool get_world_submission_for_scenario(const RID &p_scenario, WorldSubmission *r_submission) const;
    bool has_world_submission_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    // Current hint precedence is active world > homogeneous instance submissions.
    // Conflicting instance submission hints return false with source "mixed_instance_submissions".
    bool get_submission_residency_hint_for_renderer(const GaussianSplatRenderer *p_renderer,
            int32_t *r_hint, String *r_source = nullptr) const;
    SubmissionCounts get_submission_counts() const;

    Ref<GaussianSplatRenderer> get_shared_renderer(World3D *p_world);

protected:
    static void _bind_methods();

private:
    struct InstanceRecord {
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
    struct AssetRecord {
        Ref<GaussianSplatAsset> asset;
        Ref<GaussianData> data;
        uint32_t refcount = 0;
        uint32_t edited_version = 0;
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
    class InstanceStore {
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

        LocalVector<InstanceRecord> instances;
        HashMap<ObjectID, uint32_t> instance_lookup;
        uint64_t instance_generation = 1;
        uint64_t instance_asset_generation = 1;
        HashMap<uint64_t, AssetRecord> asset_records;
    };

    struct SphereEffectorRecord {
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
    class SphereEffectorStore {
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
    class SubmissionStore {
    public:
        struct WorldSubmissionRecord {
            ObjectID owner_id;
            Ref<GaussianData> gaussian_data;
            Ref<ChunkPayloadSource> payload_source;
            Vector<GaussianSplatRenderer::StaticChunk> static_chunks;
            AABB bounds;
            Dictionary metadata;
            bool has_desired_residency_hint = false;
            int32_t desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
            Dictionary desired_renderer_overrides;
            GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot renderer_restore_state;
            bool active = false;
        };

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
        static GaussianSplatRenderer::WorldSubmissionContract build_contract(
                const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_renderer_state,
                const WorldSubmissionRecord &p_record);

    private:
        WorldSubmissionRecord record;
    };

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
    class RendererLifecycleOwner {
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
        int reference_count() const { return renderer.is_null() ? 0 : renderer->get_reference_count(); }

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
    class LODCacheOwner {
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

    struct SharedWorld {
        RID scenario;
        RendererLifecycleOwner renderer_owner;
        InstanceStore instance_store;
        SphereEffectorStore sphere_effector_store;
        SubmissionStore submission_store;
        // #610 S9: the per-instance LOD-walk memoization + the walk itself.
        LODCacheOwner lod_cache_owner;
    };

    // #611: renderer-contract work captured while `world_mutex` is held and
    // executed only after it has been released.
    //
    // `apply_world_submission_contract()`, `restore_world_submission_runtime_state()`
    // and `GaussianSplatRenderer::initialize()` all reach a *blocking*
    // render-thread dispatch. Building the contract, by contrast, is pure
    // bookkeeping. So the split is: decide under the lock, dispatch outside it.
    //
    // A queued entry holds its own `Ref<GaussianSplatRenderer>`, so the renderer
    // stays alive across the unlock even if the world it came from is pruned in
    // between; it does *not* reference the `SharedWorld`, which may be gone.
    //
    // Declare this BEFORE the `ThreadOwnedMutexLock` in every caller: locals are
    // destroyed in reverse order, so the lock must be released first.
    //
    // #610 S6: every world_mutex critical section in the director's .cpp now
    // carries the enclosing `RendererContractWorkQueue` (below) rather than a bare
    // DeferredRendererWork, because most also needed the `#628` deferred-release
    // vector and the two had a fragile relative destruction order. That wrapper
    // owns this queue plus the release vector and fixes the order internally, so
    // callers declare one local instead of two ordered ones. This class stays a
    // standalone primitive — used directly by
    // tests/test_scene_director_renderer_contract_lock.h — and its own
    // declare-before-the-lock contract is unchanged.
    //
    // Public only so its bookkeeping can be tested directly (see
    // tests/test_scene_director_renderer_contract_lock.h). It is an internal
    // mechanism, not part of the node-facing API.
public:
    class DeferredRendererWork {
    public:
        enum class Kind : uint8_t {
            APPLY,
            RESTORE,
            INITIALIZE,
        };

    private:
        struct Entry {
            Ref<GaussianSplatRenderer> renderer;
            GaussianSplatRenderer::WorldSubmissionContract contract;
            GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot restore_state;
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
                const GaussianSplatRenderer::WorldSubmissionContract &p_contract);
        void queue_restore(const Ref<GaussianSplatRenderer> &p_renderer,
                const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_snapshot);
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
                const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_snapshot);
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
    class RendererContractWorkQueue {
    public:
        // --- deferred contract-work primitives (forwarded to the queue) ---
        void queue_apply(const Ref<GaussianSplatRenderer> &p_renderer,
                const GaussianSplatRenderer::WorldSubmissionContract &p_contract) {
            work.queue_apply(p_renderer, p_contract);
        }
        void queue_restore(const Ref<GaussianSplatRenderer> &p_renderer,
                const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_snapshot) {
            work.queue_restore(p_renderer, p_snapshot);
        }
        void queue_restore_first(const Ref<GaussianSplatRenderer> &p_renderer,
                const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_snapshot) {
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

private:
    static GaussianSplatSceneDirector *singleton;

    // #611: a plain `Mutex` cannot answer "does this thread already hold me?",
    // so the renderer-contract boundary below had no way to check the ordering
    // rule it depends on. `ThreadOwnedMutex` is a drop-in recursive mutex that
    // records its owner; lock it with `ThreadOwnedMutexLock`, never with Godot's
    // `MutexLock` (which binds the underlying std::mutex directly and would
    // bypass the ownership record).
    mutable GaussianSplatting::ThreadOwnedMutex world_mutex;
    // #611 PR B2: serializes submit_world_submission's three-phase
    // arbitrate-under-lock -> apply-unlocked -> commit-under-lock sequence.
    //
    // LOCK ORDER: always acquired BEFORE `world_mutex`, and never while holding
    // it. It is taken in exactly one function (submit_world_submission), so that
    // ordering is trivially consistent.
    //
    // THE RENDER THREAD NEVER ACQUIRES THIS MUTEX. That is the property which
    // keeps it out of the inversion: the render thread blocks only on
    // `world_mutex` (inside the `*_for_renderer` builders), so a main thread
    // holding this one and waiting on a render-thread dispatch cannot be waiting
    // on a thread that is waiting on it.
    //
    // WHY IT IS NEEDED AT ALL: with the apply moved outside `world_mutex`, two
    // concurrent submissions could interleave arbitrate/apply/commit and commit a
    // record whose contract was never the last one applied to the renderer. This
    // mutex makes the whole three-phase sequence atomic with respect to other
    // submissions, so re-validation on commit only has to defend against
    // NON-submission mutations (prune, renderer swap, teardown).
    //
    // Recursive (Godot's `Mutex`) rather than `BinaryMutex`: re-entering
    // submit_world_submission on one thread would break the phase logic, but it
    // must not self-deadlock while doing so.
    mutable Mutex world_submission_apply_mutex;
    // #611: counts entries into _apply_world_submission_to_renderer /
    // _restore_world_submission_renderer made while the calling thread holds
    // world_mutex. Non-zero means a blocking render-thread dispatch was issued
    // from inside the critical section the render thread itself needs.
    static SafeNumeric<uint64_t> renderer_contract_lock_violations;

    // #610 S8: WorldRegistry owns the cross-scenario `worlds` map -- the
    // HashMap<RID, SharedWorld> keyed by World3D scenario RID -- together with the
    // lookup / registration / removal / iteration operations the director's
    // world-management code performs on it. It is the boundary the register /
    // world-switch / prune / teardown paths see: callers touch only these public
    // methods, never the map, mirroring S3's InstanceStore / S4's
    // SphereEffectorStore / S5's SubmissionStore / S7's RendererLifecycleOwner.
    //
    // It takes NO lock of its own: every method is called under the director's
    // single world_mutex, preserving the one ThreadOwnedMutex ownership record the
    // renderer-contract-boundary guard depends on. It holds no back-pointer to the
    // director and reaches into no SharedWorld's store internals -- it owns only the
    // map of SharedWorlds; each world owns its own instance / sphere-effector /
    // submission / renderer components (S3-S7).
    //
    // #628 PRESERVED: the registry only vends a map entry (find) and erases it
    // (erase). The prune's deferred renderer-Ref release -- moving the last
    // Ref<GaussianSplatRenderer> OUT of the entry into a caller-owned release vector
    // BEFORE erase() so ~GaussianSplatRenderer runs after world_mutex is dropped --
    // stays verbatim in _prune_world_if_unused / teardown_world_for_scenario. The
    // registry never releases a renderer Ref itself; erase() only drops an entry
    // whose renderer was already moved out.
    class WorldRegistry {
    public:
        // --- queries ---
        bool is_empty() const { return worlds.is_empty(); }
        uint32_t size() const { return worlds.size(); }
        bool has(const RID &p_scenario) const { return worlds.has(p_scenario); }

        // Mutable / const lookup by scenario RID. Mirrors HashMap::getptr:
        // returns nullptr when no world is registered for the scenario.
        SharedWorld *find(const RID &p_scenario) { return worlds.getptr(p_scenario); }
        const SharedWorld *find(const RID &p_scenario) const { return worlds.getptr(p_scenario); }

        // --- registration / removal ---
        // Insert a fresh SharedWorld for p_scenario and return a pointer to the
        // stored entry. Mirrors the inline `worlds.insert(scenario, world);
        // worlds.getptr(scenario)` idiom verbatim -- re-fetch through the map rather
        // than trusting the insert return, so the pointer is into the stored slot.
        SharedWorld *insert(const RID &p_scenario, const SharedWorld &p_world) {
            worlds.insert(p_scenario, p_world);
            return worlds.getptr(p_scenario);
        }
        // Drop the entry for p_scenario. The caller MUST already have moved any
        // renderer Ref out of the entry into a deferred-release vector (see #628
        // above); this only removes the map slot.
        void erase(const RID &p_scenario) { worlds.erase(p_scenario); }
        // Drop every entry. Used by ~GaussianSplatSceneDirector; see
        // release_all_worlds() for the lock-safe, deferred-release equivalent used
        // while the surrounding engine is still live.
        void clear() { worlds.clear(); }

        // --- iteration (range-for over KeyValue<RID, SharedWorld>) ---
        // Forwarded so `for (KeyValue<RID, SharedWorld> &E : world_registry)` and
        // its const form read exactly as the former `... : worlds` loops.
        HashMap<RID, SharedWorld>::Iterator begin() { return worlds.begin(); }
        HashMap<RID, SharedWorld>::Iterator end() { return worlds.end(); }
        HashMap<RID, SharedWorld>::ConstIterator begin() const { return worlds.begin(); }
        HashMap<RID, SharedWorld>::ConstIterator end() const { return worlds.end(); }

    private:
        HashMap<RID, SharedWorld> worlds;
    };

    WorldRegistry world_registry;

    SharedWorld *_get_or_create_world_for_scenario(const RID &p_scenario, bool p_require_renderer = true,
            RendererContractWorkQueue *r_deferred_work = nullptr);
    SharedWorld *_get_or_create_world(World3D *p_world, bool p_require_renderer = true,
            RendererContractWorkQueue *r_deferred_work = nullptr);
    SharedWorld *_get_world_for_instance(ObjectID p_node_id, RendererContractWorkQueue *r_deferred_work = nullptr);
    SharedWorld *_find_world_for_instance(ObjectID p_node_id);
    SharedWorld *_get_world_for_effector(ObjectID p_effector_id);
    SharedWorld *_find_world_for_effector(ObjectID p_effector_id);
    SharedWorld *_find_world_for_renderer(const GaussianSplatRenderer *p_renderer);
    const SharedWorld *_find_world_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    SharedWorld *_find_world_for_world_submission(ObjectID p_owner_id);
    const SharedWorld *_find_world_for_world_submission(ObjectID p_owner_id) const;
    static void _build_sorted_sphere_effector_payload(const SharedWorld &p_world,
            LocalVector<SphereEffectorSelection> &r_out);

    // Render-thread-safe mask builder: consumes the scene-effector filter state
    // and cached ancestor chain stored on the InstanceRecord instead of reading
    // anything back from the live Node3D. The main-thread node path keeps this
    // cache fresh via update_instance_scene_effector_filter().
    static uint32_t _build_scene_effector_mask_for_record(const InstanceRecord &p_record,
            const LocalVector<SphereEffectorSelection> &p_payload);

	// Single source of truth for the SharedWorld::asset_records key. The key is
	// the FULL 64-bit ObjectID; it must never be truncated to 32 bits or two
	// assets whose ObjectIDs share the low 32 bits would alias the same record.
	static uint64_t _asset_records_key(ObjectID p_asset_object_id) {
		return p_asset_object_id;
	}

	static bool _is_world_submission_owner_live(ObjectID p_owner_id);
	// Straddles SharedWorld (reads p_world.scenario) and the record, so it stays a
	// director-level projection rather than moving into SubmissionStore. The pure
	// record-only builders (store_submission / record_has_renderable_payload /
	// build_contract) live on the store; see #610 S5.
	static void _copy_world_submission_record(const SharedWorld &p_world, const SubmissionStore::WorldSubmissionRecord &p_record,
			WorldSubmission *r_submission);
	// #611: THE RENDERER-CONTRACT BOUNDARY (instrumented).
	//
	// All three of these reach a blocking render-thread dispatch
	// (`GaussianSplatRenderer::initialize`, `set_max_splats`, `set_gaussian_data`,
	// `set_file_backed_payload_source`). The render thread can simultaneously be
	// blocked acquiring `world_mutex` inside a `*_for_renderer` builder, so
	// entering any of these with `world_mutex` held is a lock-order inversion:
	// the dispatch stalls for its full timeout and the operation is then either
	// silently dropped (`set_max_splats`) or rolled back and rejected
	// (`set_gaussian_data` returns ERR_BUSY).
	//
	// This used to be prose only. It is now checked: every entry point consults
	// `world_mutex.is_held_by_current_thread()` and counts a violation. Prefer
	// `DeferredRendererWork` over calling these under the lock.
	//
	// COMPLETENESS — PR A shipped only the first two, and `register_instance`
	// reached `GaussianSplatRenderer::initialize()` directly (blocking dispatch at
	// renderer/gaussian_splat_renderer.cpp:1613-1618), bypassing both and making
	// the counter a lower bound. `_initialize_world_renderer` closes that route:
	// it is now the only way this class calls `initialize()`. Every remaining
	// `renderer->` call in the .cpp that can dispatch is either inside one of
	// these three functions, inside `DeferredRendererWork::flush()` (which runs
	// with the lock released, by construction), or in `teardown_world_for_scenario`
	// after an explicit unlock. `tests/ci/check_renderer_contract_boundary.py`
	// fails the build if a new one appears anywhere else.
	//
	// They are non-static precisely so they can reach `world_mutex`; do not make
	// them static again without moving the check somewhere it can still run.
	void _restore_world_submission_renderer(SharedWorld &p_world,
			const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_snapshot);
	bool _apply_world_submission_to_renderer(SharedWorld &p_world, const SubmissionStore::WorldSubmissionRecord &p_record,
			const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_renderer_state);
	// #611 PR B2: the apply for `submit_world_submission`, whose result gates the
	// commit/reject decision and therefore CANNOT be deferred — a destructor has
	// no way to feed a value back into a function that already chose its return
	// value, and committing optimistically to roll back later would make that
	// `bool` a lie to GaussianSplatWorld3D.
	//
	// So instead of deferring the dispatch, the caller defers the *lock*: it
	// releases `world_mutex` before calling this and re-acquires it afterwards.
	// This takes a `Ref` and a prebuilt contract rather than a `SharedWorld &`
	// precisely because no `SharedWorld *` is valid across that gap — the world
	// may be pruned, and `worlds` may rehash, invalidating every pointer into it.
	//
	// MUST be called with `world_mutex` released. That is not merely documented:
	// the boundary check inside reports a violation if it is not, exactly as the
	// two functions above do.
	bool _apply_world_submission_contract_unlocked(const Ref<GaussianSplatRenderer> &p_renderer,
			const GaussianSplatRenderer::WorldSubmissionContract &p_contract);
	// #611 PR B1: the single route from this class to
	// `GaussianSplatRenderer::initialize()`. Applies the
	// not-initialized-and-not-pending guard, then either queues the call on
	// `r_deferred_work` (the correct path — dispatch happens after the caller
	// releases `world_mutex`) or, when no queue is supplied, reports the boundary
	// violation and runs it inline, preserving historical behaviour for any
	// caller that has not been threaded yet.
	void _initialize_world_renderer(SharedWorld &p_world, RendererContractWorkQueue *r_deferred_work);
	void _report_renderer_contract_lock_violation(const char *p_site) const;
	// #610 S2: `_should_prune_world` is the conjunction of four independent
	// per-concern predicates, one per state group a later decomposition slice
	// will own (instances, sphere effectors, world submission, renderer Ref).
	// Naming them decouples the prune policy from those stores so each store can
	// be extracted without the prune logic reaching into its internals.
	bool _world_has_no_instances(const SharedWorld &p_world) const;
	bool _world_has_no_sphere_effectors(const SharedWorld &p_world) const;
	bool _world_submission_idle(const SharedWorld &p_world) const;
	bool _world_renderer_unshared(const SharedWorld &p_world) const;
	bool _should_prune_world(const SharedWorld &p_world) const;
	// #611: prune an empty SharedWorld without releasing its
	// Ref<GaussianSplatRenderer> under world_mutex. The renderer's teardown blocks
	// on a render-thread dispatch, and the render thread can itself be blocked
	// acquiring world_mutex inside a *_for_renderer builder — dropping the last
	// renderer Ref while holding the lock is a lock-order inversion (deadlock).
	// Any renderer that would be freed here is MOVED into r_deferred_release, which
	// the caller MUST declare BEFORE its `ThreadOwnedMutexLock lock(world_mutex)` so
	// the Refs drop only after the lock has been released.
	void _prune_world_if_unused(const RID &p_scenario,
			LocalVector<Ref<GaussianSplatRenderer>> &r_deferred_release);
};

#endif // GAUSSIAN_SPLAT_SCENE_DIRECTOR_H
