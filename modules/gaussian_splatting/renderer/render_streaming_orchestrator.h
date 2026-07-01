#ifndef GAUSSIAN_RENDER_STREAMING_ORCHESTRATOR_H
#define GAUSSIAN_RENDER_STREAMING_ORCHESTRATOR_H

#include "gaussian_splat_renderer.h"
#include <cstdint>

class RenderDataOrchestrator;
class RenderDeviceOrchestrator;
class GaussianSplatSceneDirector;

struct RenderStreamingOrchestratorDependencies {
	GaussianSplatRenderer *renderer = nullptr;
	RenderDataOrchestrator *data_orchestrator = nullptr;
	RenderDeviceOrchestrator *device_orchestrator = nullptr;

	struct RuntimePorts {
		bool (GaussianSplatRenderer::*ensure_rendering_device)(const char *p_context) = &GaussianSplatRenderer::ensure_rendering_device;
		Projection (GaussianSplatRenderer::*build_cull_projection)(RenderDataRD *p_render_data, const Projection &p_projection) const = &GaussianSplatRenderer::build_cull_projection;
		bool (GaussianSplatRenderer::*validate_cull_projection_contract)(RenderDataRD *p_render_data, const Projection &p_projection,
				const Projection &p_cull_projection, const char *p_context) = &GaussianSplatRenderer::validate_cull_projection_contract;
		void (GaussianSplatRenderer::*clear_instance_pipeline_buffers)() = &GaussianSplatRenderer::clear_instance_pipeline_buffers;
		bool (GaussianSplatRenderer::*update_instance_buffer)(LocalVector<InstanceDataGPU> &p_instances,
				const GaussianSplatRenderer::PublishedInstanceAssetRemap &p_remap,
				const LocalVector<uint64_t> *p_submission_asset_ids) = &GaussianSplatRenderer::update_instance_buffer;
		void (GaussianSplatRenderer::*run_cull_sort_pipeline_frame)(RenderDataRD *p_render_data, const Transform3D &p_world_to_camera_transform,
				const Projection &p_projection, const Projection &p_render_projection, RenderSceneBuffersRD *p_render_buffers,
				bool p_has_render_data, const String &p_cull_skip_reason, const String &p_sort_skip_reason,
				GaussianSplatRenderer::RenderFallbackReason p_cull_skip_reason_code,
				GaussianSplatRenderer::RenderFallbackReason p_sort_skip_reason_code,
				bool p_set_skip_metrics, bool p_clear_cull_state_on_skip) = &GaussianSplatRenderer::run_cull_sort_pipeline_frame;
		float (GaussianSplatRenderer::*get_cull_radius_multiplier)() const = &GaussianSplatRenderer::get_cull_radius_multiplier;
		float (GaussianSplatRenderer::*get_cull_frustum_plane_slack)() const = &GaussianSplatRenderer::get_cull_frustum_plane_slack;
	};

	RuntimePorts runtime_ports;
};

class RenderStreamingOrchestrator {
public:
	explicit RenderStreamingOrchestrator(const RenderStreamingOrchestratorDependencies &p_dependencies);

	bool ensure_instance_streaming_system(const GaussianSplatRenderer::FrameBackendPlan &p_backend_plan);
	void sync_instance_pipeline_assets(GaussianStreamingSystem *p_streaming_system);
	bool render_streaming_frame(RenderDataRD *p_render_data, const Transform3D &p_camera_to_world_transform,
			const Transform3D &p_world_to_camera_transform, const Projection &p_projection,
			const Projection &p_render_projection, RenderSceneBuffersRD *p_render_buffers,
			const GaussianSplatRenderer::FrameBackendPlan &p_backend_plan);
	void tick_streaming_only(const Transform3D &p_camera_to_world_transform, const Projection &p_projection,
			const GaussianSplatRenderer::FrameBackendPlan &p_backend_plan);

	struct VisibleLODSelection {
		uint32_t residency_request_count = 0;
		bool has_instances() const { return !instances.is_empty(); }
		const LocalVector<InstanceDataGPU> &get_instances() const { return instances; }
		LocalVector<InstanceDataGPU> &_internal_get_instances() { return instances; }
		// Parallel to instances: each row's FULL 64-bit submission asset identity (the
		// collision-free key into PublishedInstanceAssetRemap). Kept here so the
		// streaming instance upload can resolve dense slots without truncating through
		// the 32-bit InstanceDataGPU::ids[0] field.
		const LocalVector<uint64_t> &get_submission_asset_ids() const { return submission_asset_ids; }
		LocalVector<uint64_t> &_internal_get_submission_asset_ids() { return submission_asset_ids; }

	private:
		LocalVector<InstanceDataGPU> instances;
		LocalVector<uint64_t> submission_asset_ids;
	};

private:
	GaussianSplatRenderer *renderer = nullptr;
	RenderDataOrchestrator *data_orchestrator = nullptr;
	RenderDeviceOrchestrator *device_orchestrator = nullptr;
	RenderStreamingOrchestratorDependencies::RuntimePorts runtime_ports;

	HashSet<uint32_t> instance_pipeline_assets;
	HashSet<uint32_t> instance_pipeline_assets_next;
	LocalVector<uint32_t> instance_pipeline_assets_to_remove;
	LocalVector<InstanceAssetRegistration> instance_pipeline_assets_cache;
	LocalVector<InstanceDataGPU> instance_pipeline_instance_cache;
	// Parallel to instance_pipeline_instance_cache: 64-bit submission asset ids used as
	// the collision-free key when resolving dense atlas slots in update_instance_buffer.
	LocalVector<uint64_t> instance_pipeline_submission_asset_ids_cache;
	HashMap<uint32_t, uint32_t> instance_pipeline_lod_mask_cache;
	HashMap<uint32_t, uint32_t> instance_pipeline_asset_versions;
	uint64_t instance_pipeline_asset_snapshot_generation = 0;
	bool instance_pipeline_asset_snapshot_shadow_only = false;
	uint32_t instance_pipeline_empty_asset_sync_streak = 0;
	uint64_t visible_lod_selection_generation = 0;
	bool visible_lod_selection_shadow_only = false;
	uint64_t streaming_bootstrap_retry_after_frame = 0;
	String streaming_bootstrap_last_error = "none";

	// Bijective projection from the FULL 64-bit submission ObjectID to a small,
	// orchestrator-owned uint32 "streaming slot id". The GaussianStreamingSystem
	// registry identity is uint32 (its _make_chunk_key packs (asset_id<<32)|chunk_id,
	// so widening it is a GPU-coupled redesign). Calling the registry with
	// static_cast<uint32_t>(object_id) would truncate the high 32 bits: two distinct
	// ObjectIDs sharing their low 32 bits would alias to the same registry slot. We
	// instead assign each distinct ObjectID its own sequential slot, which is
	// provably collision-free regardless of any engine ObjectID-layout invariant.
	// Slot 0 is reserved for the synthetic primary/world id (kPrimaryStreamingAssetId)
	// and is never handed to a real ObjectID. The reverse map + free list let
	// _release_streaming_asset_slot recycle slots on unregister so the counter does
	// not grow unbounded across asset churn.
	HashMap<uint64_t, uint32_t> streaming_asset_slot_by_object_id;
	HashMap<uint32_t, uint64_t> streaming_object_id_by_slot;
	LocalVector<uint32_t> streaming_asset_slot_free_list;
	uint32_t streaming_asset_slot_next = 1;

	uint64_t static_layout_cache_revision = 0;
	Vector<GaussianStreamingSystem::ChunkLayoutHint> static_layout_cache_hints;
	bool static_layout_cache_valid = false;
	bool static_layout_warned_non_contiguous = false;
	uint64_t static_layout_fallback_total = 0;
	uint64_t static_layout_fallback_io_total = 0;
	uint64_t static_layout_fallback_primary_total = 0;
	HashMap<String, uint64_t> static_layout_fallback_reason_counts;
	HashMap<String, uint64_t> static_layout_fallback_category_counts;
	String static_layout_fallback_last_usage = "none";
	String static_layout_fallback_last_reason = "none";
	String static_layout_fallback_last_reason_category = "other";
	String static_layout_fallback_last_context = "none";
	String static_layout_fallback_last_detail = "none";
	uint32_t static_layout_bound_asset_id = UINT32_MAX;
	Vector<GaussianStreamingSystem::ChunkLayoutHint> primary_layout_cache_hints;
	Vector<uint32_t> primary_layout_cache_source_indices;
	bool primary_layout_cache_valid = false;
	bool primary_layout_warned_invalid = false;

	VisibleLODSelection visible_lod_selection;

	// Get-or-assign the projected slot for a full 64-bit submission ObjectID
	// (mutating). Records both forward and reverse maps. The reserved primary id
	// (0) maps to slot 0; every real (non-zero) ObjectID gets a non-zero slot.
	uint32_t _project_streaming_asset_slot(uint64_t p_object_id);
	// Return the already-assigned slot for an ObjectID, or UINT32_MAX if none has
	// been assigned yet (read-only; safe to call from const-flavored build paths).
	uint32_t _peek_streaming_asset_slot(uint64_t p_object_id) const;
	// Recycle a slot previously handed out by _project_streaming_asset_slot.
	void _release_streaming_asset_slot(uint32_t p_slot);

	GaussianSplatRenderer::PublishedInstanceAssetRemap _build_streaming_instance_asset_remap(
			GaussianStreamingSystem *p_streaming_system,
			const LocalVector<InstanceAssetRegistration> &p_asset_cache,
			const LocalVector<InstanceDataGPU> &p_instances,
			const LocalVector<uint64_t> &p_submission_asset_ids,
			uint64_t p_generation) const;

	void invalidate_instance_pipeline_caches();
	bool refresh_instance_asset_snapshot(GaussianSplatSceneDirector *p_director, bool p_trace_enabled);
	const VisibleLODSelection &produce_visible_lod_selection(GaussianSplatSceneDirector *p_director,
			GaussianStreamingSystem *p_streaming_system, const Vector3 &p_camera_origin);
	void consume_visible_lod_selection_for_residency(const VisibleLODSelection &p_selection,
			GaussianStreamingSystem *p_streaming_system, bool p_trace_enabled);
};

#endif
