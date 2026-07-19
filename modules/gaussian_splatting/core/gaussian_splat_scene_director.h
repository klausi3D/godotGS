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
    // refs (asset records, world-submission record). Called by GaussianSplatWorld3D and
    // GaussianSplatNode3D from NOTIFICATION_PREDELETE so editor F6 reload (which throws the
    // SceneTree away without invoking `~GaussianSplatSceneDirector`) does not leak an entire
    // renderer's worth of GPU allocations per cycle. See gaussian_splat_scene_director.cpp:351
    // and the closing scenario_c test in test_renderer_lifetime_proof.h.
    //
    // Bypasses the `_should_prune_world` refcount>1 guard intentionally: external Refs held
    // by the about-to-be-deleted scene tree nodes will drop in their own dtors that follow
    // PREDELETE. After teardown the next register_* call rebuilds the SharedWorld lazily.
    void teardown_world_for_scenario(const RID &p_scenario);
    // Idempotent teardown of EVERY SharedWorld, equivalent to what
    // `~GaussianSplatSceneDirector` does via worlds.clear() but callable while the
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
		// otherwise alias the same SharedWorld::asset_records entry.
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

    struct SharedWorld {
        RID scenario;
        Ref<GaussianSplatRenderer> renderer;
        LocalVector<InstanceRecord> instances;
        HashMap<ObjectID, uint32_t> instance_lookup;
        uint64_t instance_generation = 1;
        uint64_t instance_asset_generation = 1;
        LocalVector<SphereEffectorRecord> sphere_effectors;
        HashMap<ObjectID, uint32_t> sphere_effector_lookup;
        uint64_t sphere_effector_generation = 1;
        uint64_t sphere_effector_registration_serial = 0;
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
        WorldSubmissionRecord world_submission;
        struct AssetRecord {
            Ref<GaussianSplatAsset> asset;
            Ref<GaussianData> data;
            uint32_t refcount = 0;
            uint32_t edited_version = 0;
        };
        HashMap<uint64_t, AssetRecord> asset_records;

        // --- Per-instance LOD-walk memoization (host-side, zero visual effect) ---
        // update_instance_lods_for_renderer() is an O(instances) walk called every
        // frame. Its result is a pure function of (camera position, every instance
        // transform/bias, the LODConfig, the hysteresis zone). instance_generation
        // already bumps on every instance add/remove/transform/param change, so we
        // can skip the whole walk when none of those inputs moved since last time.
        // Recorded AFTER the walk so the walk's own generation bump is captured.
        bool lod_walk_cache_valid = false;
        Vector3 lod_walk_last_camera_pos;
        uint64_t lod_walk_last_generation = 0;
        LODConfig lod_walk_last_config;
        float lod_walk_last_hysteresis = 0.0f;
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
    // destroyed in reverse order, so the lock must be released first. Where a
    // caller also carries a `deferred_renderer_release` vector (the #628 pattern),
    // declare this AFTER it, so the queued work runs BEFORE the renderer Refs are
    // dropped — matching the order the inline code had.
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
    HashMap<RID, SharedWorld> worlds;
    mutable HashSet<ObjectID> scene_effector_multi_match_warned_nodes;

    SharedWorld *_get_or_create_world_for_scenario(const RID &p_scenario, bool p_require_renderer = true,
            DeferredRendererWork *r_deferred_work = nullptr);
    SharedWorld *_get_or_create_world(World3D *p_world, bool p_require_renderer = true,
            DeferredRendererWork *r_deferred_work = nullptr);
    SharedWorld *_get_world_for_instance(ObjectID p_node_id, DeferredRendererWork *r_deferred_work = nullptr);
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

	static bool _populate_gaussian_data_from_asset(const Ref<GaussianSplatAsset> &p_asset, Ref<GaussianData> &r_data);
	static bool _retain_asset_record(SharedWorld &p_world, const Ref<GaussianSplatAsset> &p_asset, uint64_t p_asset_id);
	static bool _refresh_asset_record(SharedWorld &p_world, const Ref<GaussianSplatAsset> &p_asset, uint64_t p_asset_id);
	static void _release_asset_record(SharedWorld &p_world, uint64_t p_asset_id);
	static bool _is_world_submission_owner_live(ObjectID p_owner_id);
	static void _store_world_submission_record(SharedWorld::WorldSubmissionRecord &r_record, const WorldSubmission &p_submission);
	static bool _world_submission_record_has_renderable_payload(const SharedWorld::WorldSubmissionRecord &p_record);
	static void _copy_world_submission_record(const SharedWorld &p_world, const SharedWorld::WorldSubmissionRecord &p_record,
			WorldSubmission *r_submission);
	static GaussianSplatRenderer::WorldSubmissionContract _build_world_submission_contract(
			const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_renderer_state,
			const SharedWorld::WorldSubmissionRecord &p_record);
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
	bool _apply_world_submission_to_renderer(SharedWorld &p_world, const SharedWorld::WorldSubmissionRecord &p_record,
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
	void _initialize_world_renderer(SharedWorld &p_world, DeferredRendererWork *r_deferred_work);
	void _report_renderer_contract_lock_violation(const char *p_site) const;
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
