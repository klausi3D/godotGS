#include "../lod/hlod_edit.h"
#include "resident_instance_contract_publisher.h"

#include "../core/gs_vector_alloc.h"
#include "gaussian_gpu_layout.h"
#include "hlod_bundle_rows.h"
#include "hlod_drawn_node.h"
#include "hlod_world_instances.h"
#include "hlod_node_pack.h"
#include "gaussian_splat_renderer.h"
#include "gpu_sorting_config.h"
#include "instance_pipeline_contract.h"
#include "quantization_config.h"
#include "../core/streaming_quantization.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../interfaces/gpu_sorting_pipeline.h"
#include "../interfaces/output_compositor.h"
#include "tile_renderer.h"
#include "core/os/os.h"
#include "../resources/color_grading_resource.h"
#include "gs_shadow_caster.h"
#include "resident_atlas_budget.h"
#include "../core/gs_project_settings.h"
#include "core/config/project_settings.h"
#include "servers/rendering/rendering_device.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cmath>
#ifdef TESTS_ENABLED
#include "../core/gaussian_splat_world.h"
#include "../core/gaussian_splat_hlod_bake.h"
#include "../interfaces/sync_policy.h"
#include "pipeline_io_contracts.h"
#include "servers/rendering_server.h"
#endif

namespace {

using GaussianSplatting::InstancePipelineContract::InvariantViolationReason;
constexpr uint32_t kPrimaryResidentAssetId = 0u;

struct ResidentChunkDescriptor {
	AABB bounds;
	Vector<uint32_t> source_indices;
	uint32_t start_idx = 0;
	uint32_t count = 0;
	bool source_index_remapped = false;
};

struct ResidentAssetDescriptor {
	// Full 64-bit host submission identity (asset_records key / ObjectID). Must stay
	// 64-bit so it remains the collision-free key into the published remap. The dense
	// atlas slot below stays 32-bit (sequential, collision-free, GPU-facing).
	uint64_t submission_asset_id = 0;
	uint32_t dense_asset_id = 0;
	Ref<GaussianData> data;
	Vector<GaussianSplatRenderer::StaticChunk> static_chunks;
};

static uint64_t _mix_generation(uint64_t p_accum, uint64_t p_value) {
	uint64_t x = p_accum + 0x9e3779b97f4a7c15ULL;
	x ^= p_value + (x << 6) + (x >> 2);
	return x;
}

static AABB _compute_contiguous_chunk_bounds(const LocalVector<Gaussian> &p_gaussians, uint32_t p_start, uint32_t p_count) {
	if (p_count == 0 || p_start >= p_gaussians.size()) {
		return AABB();
	}

	Vector3 min_pos = p_gaussians[p_start].position;
	Vector3 max_pos = p_gaussians[p_start].position;
	const uint32_t end = MIN<uint32_t>(p_start + p_count, p_gaussians.size());
	for (uint32_t i = p_start + 1; i < end; i++) {
		const Vector3 &pos = p_gaussians[i].position;
		min_pos.x = MIN(min_pos.x, pos.x);
		min_pos.y = MIN(min_pos.y, pos.y);
		min_pos.z = MIN(min_pos.z, pos.z);
		max_pos.x = MAX(max_pos.x, pos.x);
		max_pos.y = MAX(max_pos.y, pos.y);
		max_pos.z = MAX(max_pos.z, pos.z);
	}
	Vector3 size = max_pos - min_pos;
	if (size == Vector3()) {
		size = Vector3(0.001f, 0.001f, 0.001f);
	}
	return AABB(min_pos, size);
}

static GaussianDCEncoding _resolve_data_dc_encoding(const Ref<GaussianData> &p_data) {
	if (p_data.is_null() || p_data->get_count() <= 0) {
		return GAUSSIAN_DC_ENCODING_LEGACY_BIAS;
	}
	return gaussian_get_dc_encoding(p_data->get_gaussian(0).render_meta);
}

template <typename T>
static bool _upload_typed_storage_buffer(GaussianSplatRenderer *p_renderer, RenderingDevice *p_rd, RID &r_buffer,
		uint32_t &r_buffer_size, const char *p_label, const Vector<T> &p_data) {
	if (p_data.is_empty()) {
		p_renderer->free_owned_resource(p_rd, r_buffer);
		r_buffer_size = 0;
		return false;
	}

	const uint64_t required_size_u64 = uint64_t(p_data.size()) * sizeof(T);
	static constexpr uint64_t kMaxResidentUploadBytes = uint64_t(2) * 1024 * 1024 * 1024; // 2 GB staging limit
	if (required_size_u64 > kMaxResidentUploadBytes) {
		return false;
	}
	const uint32_t required_size = uint32_t(required_size_u64);
	Vector<uint8_t> upload_bytes;
	// #798: the memcpy length is required_size (the REQUESTED count), not
	// upload_bytes.size(), so an ignored resize failure would memcpy into a null
	// destination. Fail closed exactly like the staging-limit branch above: return false
	// and leave the existing buffer/size alone (do NOT free it — the caller decides, and
	// it already clears the instance-pipeline buffers on false).
	if (!gs_resize_or_fail(upload_bytes, (int64_t)required_size,
				"ResidentInstanceContractPublisher::_upload_typed_storage_buffer")) {
		return false;
	}
	memcpy(upload_bytes.ptrw(), p_data.ptr(), required_size);

	if (!r_buffer.is_valid() || r_buffer_size != required_size) {
		p_renderer->free_owned_resource(p_rd, r_buffer);
		r_buffer = p_rd->storage_buffer_create(required_size, upload_bytes);
		if (!r_buffer.is_valid()) {
			r_buffer_size = 0;
			return false;
		}
		p_rd->set_resource_name(r_buffer, p_label);
		p_renderer->track_resource_owner(r_buffer, p_rd);
		r_buffer_size = required_size;
		return true;
	}

	p_rd->buffer_update(r_buffer, 0, required_size, upload_bytes.ptr());
	return true;
}

// #794 review: returns false ONLY on allocation failure. This used to return void, so
// an allocation failure produced an empty list that the caller could not distinguish
// from "this asset legitimately has no chunks" -- it did `continue`, published a
// successful contract, and cached the source generation, leaving the asset with a valid
// dense id but no atlas chunks: invisible, with no retry until its generation changed.
[[nodiscard]] static bool _append_chunk_descriptors_for_asset(const ResidentAssetDescriptor &p_asset,
		LocalVector<ResidentChunkDescriptor> &r_chunks) {
	const uint32_t total_count = p_asset.data.is_valid() ? uint32_t(MAX(0, p_asset.data->get_count())) : 0u;
	if (total_count == 0) {
		return true; // nothing to append is not a failure
	}

	if (!p_asset.static_chunks.is_empty()) {
		for (int chunk_idx = 0; chunk_idx < p_asset.static_chunks.size(); chunk_idx++) {
			const GaussianSplatRenderer::StaticChunk &chunk = p_asset.static_chunks[chunk_idx];
			if (chunk.indices.is_empty()) {
				continue;
			}
			uint32_t offset = 0;
			while (offset < uint32_t(chunk.indices.size())) {
				const uint32_t split_count = MIN<uint32_t>(GaussianStreamingSystem::CHUNK_SIZE,
						uint32_t(chunk.indices.size()) - offset);
				ResidentChunkDescriptor descriptor;
				descriptor.bounds = chunk.bounds;
				descriptor.count = split_count;
				descriptor.source_index_remapped = true;
				// #794: the copy loop is bounded by split_count, not by
				// source_indices.size(), so an ignored resize failure would write past
				// the end and hard-trap. Publishing a descriptor whose count says
				// split_count but whose source_indices are empty would be worse than
				// dropping it -- #793 showed exactly that shape reaching
				// buffer_update() as a null pointer with a nonzero byte count. So bail
				// out of publishing entirely rather than emit a partial chunk set.
				if (!gs_resize_or_fail(descriptor.source_indices, (int64_t)split_count,
							"ResidentInstanceContractPublisher::_append_chunk_descriptors_for_asset")) {
					r_chunks.clear();
					return false;
				}
				for (uint32_t local_idx = 0; local_idx < split_count; local_idx++) {
					descriptor.source_indices.write[local_idx] = chunk.indices[offset + local_idx];
				}
				r_chunks.push_back(descriptor);
				offset += split_count;
			}
		}
		return true;
	}

	const LocalVector<Gaussian> &gaussians = p_asset.data->get_gaussian_storage();
	for (uint32_t start = 0; start < total_count; start += GaussianStreamingSystem::CHUNK_SIZE) {
		const uint32_t count = MIN<uint32_t>(GaussianStreamingSystem::CHUNK_SIZE, total_count - start);
		ResidentChunkDescriptor descriptor;
		descriptor.start_idx = start;
		descriptor.count = count;
		descriptor.bounds = _compute_contiguous_chunk_bounds(gaussians, start, count);
		r_chunks.push_back(descriptor);
	}
	return true;
}

// The one read of the device heap for the budgets of this file. The ledger owns the limits (renderer/device_vram_ledger.h).
DeviceVramLedger &refresh_device_vram_ledger(GaussianRenderFacadeState::ResourceState &p_state, RenderingDevice *p_rd, uint64_t p_limit_clamp = 0) {
    p_state.device_vram_ledger.configure(p_rd->get_device_memory_budget(), GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT, p_limit_clamp);
    return p_state.device_vram_ledger;
}

} // namespace

namespace ResidentInstanceContractPublisher {

bool publish_resident_direct_data_contract(GaussianSplatRenderer *p_renderer, String *r_reason) {
	ERR_FAIL_NULL_V(p_renderer, false);

	if (r_reason) {
		*r_reason = String();
	}

	if (!p_renderer->ensure_rendering_device("resident_instance_contract_publish")) {
		if (r_reason) {
			*r_reason = "rendering_device_unavailable";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

	// Per-chunk quantization: the resident atlas packs the 80-byte PackedGaussianQuantized
	// layout with per-chunk bounds (GS-PERF-Q80B). The in-memory resident data lets us
	// compute exact per-chunk bounds (unlike the out-of-core streaming path), and the
	// resident atlas already carries a chunk structure with a ChunkMetaGPU per chunk.
	const bool quantize_atlas = g_quantization_config.per_chunk_quantization;

	RenderingDevice *rd = p_renderer->get_device_state().rd;
	if (rd == nullptr) {
		if (r_reason) {
			*r_reason = "rendering_device_unavailable";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

    String hlod_reason;
    bool omit_edited_world = false;
    const int hlod_result = p_renderer->try_publish_hlod_resident_contract(hlod_reason);
    {
        // Package F1 (ADR 14.5): decided AFTER the publish attempt, in the same frame. The attempt is what rebuilds a
        // released streamer from the committed snapshot, so it must run; but a frame whose route does not serve the
        // committed edits (the attempt released the route, failed, or has not rebuilt yet) must not publish the canonical
        // copy of the unedited world: nothing is drawn, and the instance-pipeline buffers (which the shadow and sort passes
        // read) are cleared so no pass keeps using an old atlas.
        // F-3: a result <= 0 means the HLOD route did not publish this frame (idle failed, sort buffers invalid, not taken...), and
        // then the canonical copy below is the UNEDITED world even where a streamer is alive: blank as well.
        String edit_blank_reason;
        const bool committed_here = p_renderer->hlod_edit_committed_here();
        GaussianSplatSceneDirector *edit_director = GaussianSplatSceneDirector::get_singleton();
        const bool blanks = p_renderer->hlod_edit_blanks_frame(edit_blank_reason);
        const GaussianSplatRenderer::HlodEditFrame edit_frame = GaussianSplatRenderer::decide_hlod_edit_frame(committed_here, blanks, hlod_result,
                edit_director != nullptr && edit_director->get_instance_count_for_renderer(p_renderer) != 0);
        if (edit_frame != GaussianSplatRenderer::HlodEditFrame::PUBLISH) {
            if (edit_blank_reason.is_empty()) {
                edit_blank_reason = GaussianSplatRenderer::compose_hlod_edit_blank_reason(p_renderer->get_resource_state().hlod_reason,
                        gs::settings::get_hlod_runtime_cut_enabled(ProjectSettings::get_singleton()));
            }
            ++p_renderer->get_resource_state().hlod_edit_blank_frames;
            // A blanked frame never reaches the tile stage: it is not active (a stale active flag of the last frame would make
            // it look like one) and it is not a tile-stage refusal either (hlod_mark_tile_stage_refused only counts frames
            // that were admitted as active and then rasterized).
            p_renderer->get_resource_state().hlod_frame_active = false;
            p_renderer->get_resource_state().hlod_reason = edit_blank_reason;
            if (r_reason) { *r_reason = edit_blank_reason; }
            // F-2: only the edited world is omitted. Other instances of the scene keep rendering through the canonical
            // publication below, which then packs without the world's own (unedited) data; with nothing else to draw the
            // frame is skipped and the buffers cleared.
            if (edit_frame == GaussianSplatRenderer::HlodEditFrame::SKIP_FRAME) {
                p_renderer->clear_instance_pipeline_buffers();
                return false;
            }
            omit_edited_world = true;
            if (hlod_result != 0) { p_renderer->release_hlod_resources(); } // Never keep an unedited HLOD bundle published.
        }
    }
    if (hlod_result != 0 && !omit_edited_world) {
        if (r_reason) { *r_reason = hlod_reason; }
        return hlod_result > 0;
    }
    {
        // Package E1: a V4 paged world has no canonical payload to publish instead; the frame is skipped, with the
        // reason the HLOD route gave (also in the stats as hlod_reason).
        const auto &paged_state = p_renderer->get_resource_state();
        if (!omit_edited_world && paged_state.hlod_snapshot.is_valid() && paged_state.hlod_snapshot->is_paged()) {
            if (r_reason) { *r_reason = paged_state.hlod_reason.is_empty() ? String("hlod_paged_route_unavailable") : paged_state.hlod_reason; }
            return false;
        }
    }

	// Resident atlas VRAM budget (#321 follow-up). A default GaussianSplatNode3D is
	// resident-only by contract, so an over-large atlas cannot fall back to streaming and
	// would OOM / device-lost on a low-end GPU. Clamp the packed atlas to the device VRAM
	// budget (or an explicit project override) and pack an importance-ordered subset below
	// instead. Unknown/UMA budgets and capable GPUs resolve to the 2 GB staging ceiling, so
	// the effective cap == the staging ceiling and nothing below changes.
	const uint32_t resident_atlas_override_mb = gs::settings::get_uint(ProjectSettings::get_singleton(),
			"rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb", 0u);
	const uint64_t effective_atlas_cap_bytes =
			refresh_device_vram_ledger(p_renderer->get_resource_state(), rd).atlas_limit(resident_atlas_override_mb);

	// The edited world's own data is the UNEDITED one: omitted from the canonical atlas (F-2), the rest of the scene stays.
	const Ref<GaussianData> primary_data = omit_edited_world ? Ref<GaussianData>() : p_renderer->get_scene_state().gaussian_data;
	const bool has_primary_data = primary_data.is_valid() && primary_data->get_count() > 0;

	LocalVector<ResidentAssetDescriptor> assets;
	assets.reserve(8);

	ResidentAssetDescriptor primary_asset;
	primary_asset.submission_asset_id = kPrimaryResidentAssetId;
	primary_asset.dense_asset_id = 0;
	primary_asset.data = primary_data;
	if (has_primary_data) {
		primary_asset.static_chunks = p_renderer->get_static_chunks();
	}
	assets.push_back(primary_asset);

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	LocalVector<InstanceAssetRegistration> instance_assets;
	if (director != nullptr) {
		// Stable-superset collection: include every registered asset regardless of any
		// instance's current visibility or shadow-casting state. Visibility/casts_shadow
		// flips therefore never mutate atlas membership and never trigger a full atlas
		// repack via the atlas_generation gate below. The per-instance shadow filter is
		// still applied later by build_instance_buffer_for_renderer when populating the
		// instance buffer rows.
		director->collect_registered_assets_for_renderer(p_renderer, instance_assets);
	}
	std::sort(instance_assets.ptr(), instance_assets.ptr() + instance_assets.size(),
			[](const InstanceAssetRegistration &a, const InstanceAssetRegistration &b) {
				return a.asset_id < b.asset_id;
			});

	uint32_t next_dense_id = 1;
	for (const InstanceAssetRegistration &entry : instance_assets) {
		if (entry.asset_id == 0 || entry.data.is_null()) {
			continue;
		}
		ResidentAssetDescriptor asset;
		asset.submission_asset_id = entry.asset_id;
		asset.dense_asset_id = next_dense_id++;
		asset.data = entry.data;
		assets.push_back(asset);
	}

	// Generation split: atlas_generation hashes only inputs that affect packed atlas content
	// (asset list + per-asset content_revision + primary data + static chunk topology +
	// quantization config). instance_generation hashes everything else (per-instance state,
	// effectors, grading defaults, shadow filter, max_splats). The full early-out below
	// requires both to match the cached source_generation; if only instance_generation
	// changed, we skip the expensive atlas pack/upload and re-run only the cheap
	// per-instance buffer + grading buffer paths.
	uint64_t atlas_generation = 0x6a09e667f3bcc909ULL;
	atlas_generation = _mix_generation(atlas_generation, has_primary_data ? uint64_t(primary_data->get_instance_id()) : 0ULL);
	atlas_generation = _mix_generation(atlas_generation, has_primary_data ? primary_data->get_content_revision() : 0ULL);
	const Ref<GPUCuller> &gpu_culler = p_renderer->get_subsystem_state().gpu_culler;
	atlas_generation = _mix_generation(atlas_generation,
			gpu_culler.is_valid() ? gpu_culler->get_state().static_chunks_revision : 0ULL);
	for (const ResidentAssetDescriptor &asset : assets) {
		atlas_generation = _mix_generation(atlas_generation, asset.submission_asset_id);
		atlas_generation = _mix_generation(atlas_generation, asset.data.is_valid() ? uint64_t(asset.data->get_instance_id()) : 0ULL);
		atlas_generation = _mix_generation(atlas_generation, asset.data.is_valid() ? asset.data->get_content_revision() : 0ULL);
	}
	// Only perturb the atlas hash when the VRAM cap actually differs from the staging
	// ceiling (low-end device budget or an explicit override). On capable GPUs the cap
	// equals the ceiling, so the hash stays byte-identical to pre-#321 builds and there is
	// no one-time repack on deploy. The cap is a stable per-device fixpoint, so once mixed
	// it does not cause per-frame repacks.
	if (effective_atlas_cap_bytes != ResidentAtlasBudget::kResidentStagingCeilingBytes) {
		atlas_generation = _mix_generation(atlas_generation, effective_atlas_cap_bytes);
	}
	// Quantization config changes the PACKED ATLAS CONTENT (80-byte quantized layout +
	// the per-chunk quant params) vs the 144-byte raw layout, so it must perturb the
	// atlas hash — the "quantization config" input the comment above lists. Without this,
	// flipping quantization at runtime (e.g. a quality-tier switch to a quantized tier)
	// leaves atlas_changed false and the pack/upload is skipped, so the toggle is silently
	// ignored until an unrelated content/topology change lands.
	atlas_generation = _mix_generation(atlas_generation, quantize_atlas ? 1ULL : 0ULL);
	if (quantize_atlas) {
		atlas_generation = _mix_generation(atlas_generation, uint64_t(MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX)));
		atlas_generation = _mix_generation(atlas_generation, uint64_t(MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX)));
		atlas_generation = _mix_generation(atlas_generation, g_quantization_config.quantize_scales ? 1ULL : 0ULL);
	}

	uint64_t instance_generation = 0xbb67ae8584caa73bULL;
	if (director != nullptr) {
		instance_generation = _mix_generation(instance_generation, director->get_instance_generation_for_renderer(p_renderer));
		instance_generation = _mix_generation(instance_generation, director->get_sphere_effector_generation_for_renderer(p_renderer));
	}
	// Mix the renderer-wide grading-defaults counter so resident direct-data
	// grading rows (rows with no per-instance grading ref in director-less
	// direct-data flows) force a republish when the renderer's default
	// ColorGradingResource is swapped or mutated in place. The streaming
	// orchestrator already consumes this atomic in its fingerprint; the
	// resident publisher must do the same or these grading rows stay stale
	// until an unrelated content/topology change lands.
	instance_generation = _mix_generation(instance_generation,
			p_renderer->get_resource_state().instance_grading_defaults_generation.load(std::memory_order_relaxed));
	instance_generation = _mix_generation(instance_generation, p_renderer->is_shadow_instance_filter_enabled() ? 1ULL : 0ULL);
	instance_generation = _mix_generation(instance_generation, uint64_t(p_renderer->get_performance_settings().max_splats));

	const uint64_t source_generation = _mix_generation(atlas_generation, instance_generation);

	{
		const GaussianSplatRenderer::ResourceState &resource_state = p_renderer->get_resource_state();
		const GaussianRenderPipeline::InstancePipelineBuffers &published_buffers = p_renderer->get_instance_pipeline_buffers();
		auto owner_ok = [&](const RID &p_rid) {
			return !p_rid.is_valid() || p_renderer->get_resource_owner(p_rid, rd) == rd;
		};
		if (p_renderer->get_instance_backend_policy() == GaussianRenderPipeline::InstanceBackendPolicy::RESIDENT &&
				p_renderer->has_instance_pipeline_buffers() &&
				p_renderer->has_instance_asset_remap() &&
				resource_state.instance_pipeline_contract_generation == source_generation &&
				resource_state.instance_pipeline_upload_generation == source_generation &&
				GaussianSplatting::InstancePipelineContract::has_atlas_buffers(published_buffers) &&
				GaussianSplatting::InstancePipelineContract::has_cull_buffers(published_buffers) &&
				GaussianSplatting::InstancePipelineContract::has_sort_buffers(published_buffers) &&
				GaussianSplatting::InstancePipelineContract::has_raster_buffers(published_buffers) &&
				owner_ok(resource_state.instance_visible_chunk_buffer) &&
				owner_ok(resource_state.instance_splat_ref_buffer) &&
				owner_ok(resource_state.instance_counter_buffer) &&
				owner_ok(resource_state.instance_chunk_dispatch_buffer) &&
				owner_ok(resource_state.instance_indirect_count_buffer) &&
				owner_ok(resource_state.instance_count_buffer)) {
			return true;
		}
	}

	LocalVector<InstanceDataGPU> instances;
	// Parallel to `instances`: each instance's FULL 64-bit submission asset identity,
	// the collision-free key into the published remap. Threaded into
	// update_instance_buffer() so two assets whose ObjectIDs collide in the low 32 bits
	// resolve to distinct dense atlas slots instead of aliasing.
	LocalVector<uint64_t> instance_submission_asset_ids;
	// Director "owns" this renderer's instance set when there's a world
	// submission for it. In that case an empty `instances` result is
	// intentional — e.g. a shadow-filtered pass that legitimately produced
	// no shadow casters, or a frame where every instance was visibility-
	// culled — and we must NOT fabricate a primary identity instance.
	// Doing so would draw the entire submission asset into shadow maps or
	// resurrect culled scenes. The bootstrap below is only for standalone
	// direct-data renderers (tests, tools, editor preview that bind raw
	// `set_gaussian_data()` with no world submission).
	const bool director_owns_instance_set = director != nullptr &&
			director->has_world_submission_for_renderer(p_renderer);
	if (director != nullptr) {
		director->build_instance_buffer_for_renderer(p_renderer, instances,
				p_renderer->is_shadow_instance_filter_enabled(), &instance_submission_asset_ids);
	}
	if (instances.is_empty() && has_primary_data && !director_owns_instance_set) {
		InstanceDataGPU bootstrap_instance = {};
		bootstrap_instance.rotation[3] = 1.0f;
		bootstrap_instance.inv_rotation[3] = 1.0f;
		bootstrap_instance.translation_scale[3] = 1.0f;
		bootstrap_instance.params[0] = 1.0f;
		bootstrap_instance.params[1] = 1.0f;
		bootstrap_instance.params[2] = 1.0f;
		bootstrap_instance.wind_params[3] = 1.0f;
		bootstrap_instance.effect_params[0] = 1.0f;
		bootstrap_instance.effect_params[1] = 1.0f;
		bootstrap_instance.ids[0] = kPrimaryResidentAssetId;
		bootstrap_instance.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY |
				GS_INSTANCE_FLAG_SCALE_IDENTITY |
				GS_INSTANCE_FLAG_TRANSLATION_ZERO;
		// No node states the training mode here: use the project default (#1173).
		if (gs::settings::get_antialiasing_compensation_default(ProjectSettings::get_singleton())) {
			bootstrap_instance.ids[1] |= GS_INSTANCE_FLAG_ANTIALIASED;
		}
		instances.push_back(bootstrap_instance);
		instance_submission_asset_ids.push_back(uint64_t(kPrimaryResidentAssetId));
	}

	if (instances.is_empty()) {
		if (r_reason) {
			*r_reason = "resident_no_instances";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

	GaussianSplatRenderer::ResourceState &resource_state = p_renderer->get_resource_state();

	// Fast path: when atlas content is unchanged we skip the expensive pack_gaussians_range
	// loop and the four atlas storage-buffer uploads, falling through to the cheap per-instance
	// buffer + grading buffer refresh below. This is what makes per-instance churn (visibility
	// flips, transform tweens on interactables, wind animation, opacity changes, color grading
	// edits) cost ~1ms instead of repacking the whole resident atlas (which used to cost
	// hundreds of ms on dense scenes).
	//
	// Atlas buffer ownership must also match the current RenderingDevice. After a device
	// migration/reset the cached atlas RIDs would still pass the generation check but reference
	// freed resources from the previous device, and the contract would publish dead RIDs. The
	// cull/sort/raster buffers below have their own ensure_owner() remediation; the atlas
	// buffers do not, so we force a full repack here when any atlas RID has been re-owned.
	auto atlas_owner_ok = [&](const RID &p_rid) {
		return !p_rid.is_valid() || p_renderer->get_resource_owner(p_rid, rd) == rd;
	};
	const bool atlas_buffers_owned = atlas_owner_ok(resource_state.resident_atlas_gaussian_buffer) &&
			atlas_owner_ok(resource_state.resident_asset_meta_buffer) &&
			atlas_owner_ok(resource_state.resident_chunk_meta_buffer) &&
			atlas_owner_ok(resource_state.resident_asset_chunk_index_buffer);
	const bool atlas_changed = resource_state.instance_pipeline_atlas_generation != atlas_generation ||
			!p_renderer->has_instance_pipeline_buffers() ||
			!atlas_buffers_owned;

	uint32_t atlas_gaussian_count = 0;
	uint32_t atlas_max_chunk_count_per_asset = 0;
	uint32_t atlas_max_chunk_splats = 0;

	if (atlas_changed) {
		// If the atlas RIDs survived a device migration their cached values still pass the size
		// check inside _upload_typed_storage_buffer(), which would then call buffer_update() on
		// a RID that belongs to the old RenderingDevice. Drop our references to any foreign-
		// owned atlas RIDs first so the upload path recreates them on the current device. We do
		// not call free_owned_resource() because the previous device owns them and is responsible
		// for its own teardown -- mirrors the ensure_owner() pattern used for the cull/sort
		// buffers further down.
		if (!atlas_buffers_owned) {
			auto reset_foreign_atlas_rid = [&](RID &r_buffer, uint32_t &r_size) {
				if (r_buffer.is_valid() && p_renderer->get_resource_owner(r_buffer, rd) != rd) {
					r_buffer = RID();
					r_size = 0;
				}
			};
			reset_foreign_atlas_rid(resource_state.resident_atlas_gaussian_buffer,
					resource_state.resident_atlas_gaussian_buffer_size);
			reset_foreign_atlas_rid(resource_state.resident_asset_meta_buffer,
					resource_state.resident_asset_meta_buffer_size);
			reset_foreign_atlas_rid(resource_state.resident_chunk_meta_buffer,
					resource_state.resident_chunk_meta_buffer_size);
			reset_foreign_atlas_rid(resource_state.resident_asset_chunk_index_buffer,
					resource_state.resident_asset_chunk_index_buffer_size);
		}

		// Estimate the total source size, then decide the clamp plan. A default resident
		// GaussianSplatNode3D has no streaming fallback (resident-only by contract), so an
		// over-budget atlas is THINNED to fit rather than rejected -- rejecting would black-frame
		// the node under the #351 single-route invariant. The thinned atlas is hard-capped at
		// target_keep (<= effective cap <= the 2 GB per-buffer staging limit) by the global budget
		// below, so it always fits the upload regardless of source size. We only reject a source
		// beyond a generous COST ceiling: the slow-path pack loop still scans the whole source
		// once (generation-gated), so an absurdly large single resident asset (use streaming for
		// that) is bounded here.
		ResidentAtlasBudget::SubsetPlan subset_plan;
		{
			static constexpr uint64_t kResidentStagingUploadBytes = uint64_t(2) * 1024 * 1024 * 1024;
			static constexpr uint64_t kMaxResidentClampSourceBytes = uint64_t(4) * kResidentStagingUploadBytes;
			uint64_t total_gaussians = 0;
			for (const ResidentAssetDescriptor &asset : assets) {
				if (asset.data.is_valid()) {
					total_gaussians += uint64_t(asset.data->get_count());
				}
			}
			if (total_gaussians * sizeof(PackedGaussian) > kMaxResidentClampSourceBytes) {
				if (r_reason) {
					*r_reason = "resident_dataset_exceeds_clamp_source_limit";
				}
				// Do NOT clear instance pipeline buffers here — the streaming
				// orchestrator may have already published its own atlas/cull
				// buffers.  Clearing them would force MISSING_CULL_INPUTS on
				// every subsequent frame, preventing the streaming path from
				// ever becoming valid.
				return false;
			}
			// Decide whether the atlas fits the device VRAM budget, and if not, the global keep
			// ratio + target for the importance-ordered subset packed per chunk below. A source
			// above the staging limit clamps down to target_keep <= staging limit and renders
			// reduced instead of black-framing. The packed size drives how many splats fit the
			// cap, so the quantized (80 B) atlas must budget at 80 B — otherwise the clamp
			// over-thins it as if it were 144 B and the quantization VRAM win is thrown away.
			const uint32_t packed_atlas_stride = quantize_atlas
					? uint32_t(sizeof(PackedGaussianQuantized))
					: uint32_t(sizeof(PackedGaussian));
			subset_plan = ResidentAtlasBudget::compute_subset_plan(total_gaussians,
					effective_atlas_cap_bytes, packed_atlas_stride);
		}

		Vector<AssetMetaGPU> asset_meta_cpu;
		asset_meta_cpu.resize(next_dense_id);
		Vector<AssetChunkIndexGPU> asset_chunk_index_cpu;
		Vector<ChunkMetaGPU> chunk_meta_cpu;
		Vector<PackedGaussian> atlas_gaussian_cpu;
		// Parallel quantized atlas + per-chunk bounds, populated only when quantize_atlas.
		// The 144-byte atlas_gaussian_cpu stays empty in that case (and vice-versa); exactly
		// one is uploaded, keeping the non-quantized path byte-identical.
		Vector<PackedGaussianQuantized> atlas_gaussian_quantized_cpu;
		Vector<ChunkQuantizationGPU> quantization_gpu_cpu;

		uint32_t max_chunk_count_per_asset = 0;
		uint32_t max_chunk_splats = 0;
		// Hard global budget for the importance-LOD clamp. The per-chunk thinning below keeps
		// at least one splat from each non-empty chunk for coverage, which can overshoot the
		// device VRAM target on datasets with many tiny chunks; capping the cumulative kept
		// count here guarantees the resident atlas never exceeds the budget (Codex #420).
		// UINT32_MAX == unbounded when the atlas is not being thinned.
		uint32_t resident_remaining_budget = subset_plan.reduced
				? uint32_t(MIN<uint64_t>(subset_plan.target_keep, uint64_t(UINT32_MAX)))
				: UINT32_MAX;

		for (uint32_t asset_index = 0; asset_index < assets.size(); asset_index++) {
			const ResidentAssetDescriptor &asset = assets[asset_index];
			asset_meta_cpu.write[asset.dense_asset_id] = AssetMetaGPU();

			if (asset.data.is_null() || asset.data->get_count() <= 0) {
				continue;
			}

			LocalVector<ResidentChunkDescriptor> chunk_descriptors;
			// #794 review: an allocation failure here must NOT be mistaken for "this
			// asset has no chunks". Continuing past it published a successful contract
			// and cached the source generation, leaving the asset with a valid dense id
			// and no atlas chunks -- invisible, and not retried until its generation
			// changed. Fail the whole publish, matching how this function's other
			// allocation limits behave.
			if (!_append_chunk_descriptors_for_asset(asset, chunk_descriptors)) {
				if (r_reason) {
					*r_reason = "resident_chunk_descriptor_allocation_failed";
				}
				// Deliberately not clearing the instance pipeline buffers, for the same
				// reason as the clamp-source-limit branch above: the streaming
				// orchestrator may already have published its own atlas/cull buffers,
				// and clearing them would force MISSING_CULL_INPUTS every later frame.
				return false;
			}
			if (chunk_descriptors.is_empty()) {
				continue;
			}

			AssetMetaGPU asset_meta = {};
			asset_meta.lod_count = 1;
			asset_meta.sh_degree = asset.data->get_sh_degree();
			asset_meta.flags = gs_pack_asset_gpu_flags(asset.data->get_2d_mode(), _resolve_data_dc_encoding(asset.data));
			const AABB asset_bounds = asset.data->get_aabb();
			const Vector3 asset_center = asset_bounds.get_center();
			const Vector3 asset_half = asset_bounds.size * 0.5f;
			asset_meta.bounds_center_local[0] = asset_center.x;
			asset_meta.bounds_center_local[1] = asset_center.y;
			asset_meta.bounds_center_local[2] = asset_center.z;
			asset_meta.bounds_radius_local = asset_half.length();
			asset_meta.chunk_index_base = asset_chunk_index_cpu.size();
			// chunk_index_count / lod_ranges AND max_chunk_count_per_asset are finalized AFTER
			// the chunk loop: importance thinning under the global budget may drop whole chunks
			// (0 kept), so both the asset's chunk count and the dispatch / auxiliary-buffer sizing
			// must reflect the chunks actually emitted, not chunk_descriptors.size().

			// Give this asset a share of the keep budget proportional to its splat count so a
			// later (e.g. currently visible/casting) asset cannot be starved to zero chunks by
			// an earlier large asset in the stable superset. Visibility-independent, so the atlas
			// stays stable across visibility flips (Codex #420).
			uint32_t asset_remaining_budget = resident_remaining_budget;
			if (subset_plan.reduced) {
				asset_remaining_budget = ResidentAtlasBudget::resident_asset_budget(subset_plan.target_keep,
						uint64_t(asset.data->get_count()), subset_plan.source_count, resident_remaining_budget);
			}
			const uint32_t asset_budget_start = asset_remaining_budget;

			for (uint32_t chunk_index = 0; chunk_index < chunk_descriptors.size(); chunk_index++) {
				const ResidentChunkDescriptor &descriptor = chunk_descriptors[chunk_index];
				LocalVector<Gaussian> gaussian_snapshot;
				LocalVector<Vector3> sh_high_order_snapshot;
				uint32_t sh_first_order = 0;
				uint32_t sh_high_order = 0;
				bool captured = false;
				if (descriptor.source_index_remapped) {
					captured = asset.data->capture_indexed_chunk_snapshot(descriptor.source_indices.ptr(), descriptor.count,
							gaussian_snapshot, sh_high_order_snapshot, sh_first_order, sh_high_order);
				} else {
					captured = asset.data->capture_chunk_snapshot(descriptor.start_idx, descriptor.count,
							gaussian_snapshot, sh_high_order_snapshot, sh_first_order, sh_high_order);
				}
				if (!captured || gaussian_snapshot.size() != descriptor.count) {
					if (r_reason) {
						*r_reason = "resident_chunk_snapshot_failed";
					}
					p_renderer->clear_instance_pipeline_buffers();
					return false;
				}

				// Importance-ordered LOD: when the atlas is over the VRAM budget, keep the top
				// splats of THIS chunk (by the culler's opacity*scale importance) so every
				// spatial region keeps its most important splats -- preserves coverage on
				// real-scan content while the global budget below bounds the total. Compacts the
				// gaussian payload and its parallel high-order SH block in place, so sh_coeffs is
				// taken AFTER compaction.
				uint32_t chunk_pack_count = descriptor.count;
				if (subset_plan.reduced) {
					chunk_pack_count = ResidentAtlasBudget::compact_chunk_by_importance(
							gaussian_snapshot, sh_high_order_snapshot, sh_high_order,
							subset_plan.keep_ratio, asset_remaining_budget);
					if (chunk_pack_count == 0) {
						// Global VRAM budget exhausted (or this chunk's quota rounded to 0):
						// drop the chunk rather than emit a zero-splat ChunkMeta.
						continue;
					}
				}

				SHCompressionMetrics sh_metrics;
				const Vector3 *sh_coeffs = sh_high_order_snapshot.is_empty() ? nullptr : sh_high_order_snapshot.ptr();
				// #1158: a degree-3 asset loses SH coefficients to the atlas slot count; say so
				// once per asset and layout instead of rendering partial bands silently.
				gs_warn_sh_layout_truncation_once(**asset.data, sh_first_order, sh_high_order, quantize_atlas);
				// atlas_base is the SPLAT index into the atlas buffer (the shader indexes
				// gaussians[atlas_base + i]); it advances by chunk in whichever atlas is active.
				uint32_t atlas_base = 0;
				// quant_id: the global chunk index the shader uses to dereference BOTH the
				// quantization buffer and this chunk's ChunkMeta — it is exactly the position
				// this chunk_meta takes in chunk_meta_cpu.
				const uint32_t quant_id = chunk_meta_cpu.size();

				if (quantize_atlas) {
					if (quant_id > 0xFFFFu) {
						// chunk_id is a uint16 field in PackedGaussianQuantized; an atlas with
						// more than 65535 chunks cannot address its per-chunk bounds. Fail the
						// publish (all-or-nothing) so the caller keeps the legacy path.
						if (r_reason) {
							*r_reason = "resident_quantization_chunk_overflow";
						}
						p_renderer->clear_instance_pipeline_buffers();
						return false;
					}
					// Per-chunk bounds MUST be computed on the THINNED snapshot (the exact splats
					// uploaded), not descriptor.count, or the shader would dequantize against
					// bounds that don't match the stored positions.
					// Scale quantization is MANDATORY here: the 80-byte layout has no unquantized
					// scale field, and the shader substitutes scale=vec3(1.0) when scale_bits==0
					// (which renders every splat as a unit blob). g_quantization_config.quantize_scales
					// only chooses the bit depth story for callers that CAN fall back to FP32 scale;
					// the resident quantized atlas cannot, so scales are always quantized.
					// Clamp to the uint16 slot width: position_bits is project-settable up to 24,
					// which the 80-byte layout cannot store (see GS_QUANTIZED_BITS_MAX). The
					// clamped value flows into ChunkQuantizationGPU.position_bits too, so CPU
					// packing and GPU dequantization agree.
					const uint32_t q_position_bits = MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX);
					const uint32_t q_scale_bits = MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX);
					ChunkQuantizationInfo chunk_quant;
					chunk_quant.compute_from_gaussians(gaussian_snapshot, 0, chunk_pack_count,
							q_position_bits, q_scale_bits,
							/*quantize_scale=*/true);

					atlas_base = atlas_gaussian_quantized_cpu.size();
					const int prev_size = atlas_gaussian_quantized_cpu.size();
					if (atlas_gaussian_quantized_cpu.resize(prev_size + int(chunk_pack_count)) != OK) {
						if (r_reason) {
							*r_reason = "resident_quantized_atlas_alloc_failed";
						}
						p_renderer->clear_instance_pipeline_buffers();
						return false;
					}
					pack_gaussians_range_quantized(gaussian_snapshot, 0, chunk_pack_count,
							chunk_quant, uint16_t(quant_id),
							atlas_gaussian_quantized_cpu.ptrw() + prev_size, sh_metrics, sh_coeffs,
							sh_first_order, sh_high_order);

					ChunkQuantizationGPU quant_gpu = {};
					quant_gpu.position_min[0] = chunk_quant.position_min.x;
					quant_gpu.position_min[1] = chunk_quant.position_min.y;
					quant_gpu.position_min[2] = chunk_quant.position_min.z;
					quant_gpu.position_bits = chunk_quant.position_bits;
					quant_gpu.position_range[0] = chunk_quant.position_range.x;
					quant_gpu.position_range[1] = chunk_quant.position_range.y;
					quant_gpu.position_range[2] = chunk_quant.position_range.z;
					quant_gpu.scale_bits = chunk_quant.scales_quantized ? chunk_quant.scale_bits : 0u;
					quant_gpu.scale_min[0] = chunk_quant.scale_min.x;
					quant_gpu.scale_min[1] = chunk_quant.scale_min.y;
					quant_gpu.scale_min[2] = chunk_quant.scale_min.z;
					quant_gpu.start_index = atlas_base;
					quant_gpu.scale_range[0] = chunk_quant.scale_range.x;
					quant_gpu.scale_range[1] = chunk_quant.scale_range.y;
					quant_gpu.scale_range[2] = chunk_quant.scale_range.z;
					quant_gpu.count = chunk_pack_count;
					quantization_gpu_cpu.push_back(quant_gpu);
				} else {
					Vector<PackedGaussian> packed_chunk;
					// #787: abort the publish if the chunk could not be packed. Continuing would
					// append zero records while chunk_meta.splat_count below still reports
					// chunk_pack_count, publishing atlas metadata that points at splats which
					// were never written. Mirrors the quantized branch's alloc-failure handling.
					if (!pack_gaussians_range(gaussian_snapshot, 0, chunk_pack_count, packed_chunk, sh_metrics, sh_coeffs,
								sh_first_order, sh_high_order) ||
							(uint32_t)packed_chunk.size() != chunk_pack_count) {
						if (r_reason) {
							*r_reason = "resident_atlas_pack_failed";
						}
						// Drop the previously published contract like every other failure path
						// in this function does. Leaving it live keeps has_instance_pipeline_-
						// buffers() true and the old atlas/cull/sort RIDs allocated while the
						// new contract is rejected, so later frames retry the same pack against
						// the same memory pressure that just failed.
						p_renderer->clear_instance_pipeline_buffers();
						return false;
					}

					atlas_base = atlas_gaussian_cpu.size();
					for (int i = 0; i < packed_chunk.size(); i++) {
						atlas_gaussian_cpu.push_back(packed_chunk[i]);
					}
				}

				ChunkMetaGPU chunk_meta = {};
				chunk_meta.atlas_base = atlas_base;
				chunk_meta.splat_count = chunk_pack_count;
				// Per-chunk quantization: quant_base is the chunk's index into the quantization
				// buffer (== quant_id == this chunk's ChunkMeta index); the shader reads
				// quantization_buffer.chunks[quant_base]. quant_count is 1 (one bounds set/chunk).
				chunk_meta.quant_base = quantize_atlas ? quant_id : 0u;
				chunk_meta.quant_count = quantize_atlas ? 1u : 0u;
				const Vector3 chunk_center = descriptor.bounds.get_center();
				const Vector3 chunk_half = descriptor.bounds.size * 0.5f;
				chunk_meta.bounds_center_local[0] = chunk_center.x;
				chunk_meta.bounds_center_local[1] = chunk_center.y;
				chunk_meta.bounds_center_local[2] = chunk_center.z;
				chunk_meta.bounds_radius_local = chunk_half.length();
				chunk_meta.asset_id = asset.dense_asset_id;
				chunk_meta.lod_level = 0;
				chunk_meta.flags = asset_meta.flags;
				chunk_meta.sh_limit = CLAMP(asset.data->get_sh_degree(), 0u, 4u);
				chunk_meta_cpu.push_back(chunk_meta);

				AssetChunkIndexGPU chunk_index_gpu = {};
				chunk_index_gpu.chunk_id = chunk_meta_cpu.size() - 1;
				asset_chunk_index_cpu.push_back(chunk_index_gpu);
				max_chunk_splats = MAX(max_chunk_splats, chunk_pack_count);
			}

			// Return this asset's unspent budget to the global pool so later assets can use it.
			if (subset_plan.reduced) {
				resident_remaining_budget -= (asset_budget_start - asset_remaining_budget);
			}

			// Finalize the asset's chunk-index range from the chunks actually emitted (some may
			// have been dropped to 0 splats by the importance-LOD budget above), and size
			// Stage-A dispatch + the visible-chunk/splat-ref/sort buffers from that emitted count
			// so the clamp shrinks the auxiliary buffers too, not just the atlas (Codex #420).
			asset_meta.chunk_index_count = asset_chunk_index_cpu.size() - asset_meta.chunk_index_base;
			asset_meta.lod_ranges[0].base = asset_meta.chunk_index_base;
			asset_meta.lod_ranges[0].count = asset_meta.chunk_index_count;
			max_chunk_count_per_asset = MAX<uint32_t>(max_chunk_count_per_asset, asset_meta.chunk_index_count);
			asset_meta_cpu.write[asset.dense_asset_id] = asset_meta;
		}

		const uint32_t atlas_splat_total = quantize_atlas
				? uint32_t(atlas_gaussian_quantized_cpu.size())
				: uint32_t(atlas_gaussian_cpu.size());
		if (atlas_splat_total == 0) {
			if (r_reason) {
				*r_reason = "resident_atlas_empty";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}

		// Atlas gaussian buffer: exactly one of the two layouts is uploaded. The quantized
		// path also uploads the per-chunk bounds buffer the shader dereferences by quant_id.
		bool atlas_upload_ok = quantize_atlas
				? _upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_atlas_gaussian_buffer,
						resource_state.resident_atlas_gaussian_buffer_size, "GS_ResidentAtlasGaussians", atlas_gaussian_quantized_cpu)
				: _upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_atlas_gaussian_buffer,
						resource_state.resident_atlas_gaussian_buffer_size, "GS_ResidentAtlasGaussians", atlas_gaussian_cpu);
		if (!atlas_upload_ok ||
				!_upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_asset_meta_buffer,
					resource_state.resident_asset_meta_buffer_size, "GS_ResidentAssetMeta", asset_meta_cpu) ||
				!_upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_chunk_meta_buffer,
					resource_state.resident_chunk_meta_buffer_size, "GS_ResidentChunkMeta", chunk_meta_cpu) ||
				!_upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_asset_chunk_index_buffer,
					resource_state.resident_asset_chunk_index_buffer_size, "GS_ResidentAssetChunkIndex", asset_chunk_index_cpu)) {
			if (r_reason) {
				*r_reason = "resident_dataset_upload_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		if (quantize_atlas) {
			if (!_upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_quantization_buffer,
						resource_state.resident_quantization_buffer_size, "GS_ResidentQuantization", quantization_gpu_cpu)) {
				if (r_reason) {
					*r_reason = "resident_quantization_upload_failed";
				}
				p_renderer->clear_instance_pipeline_buffers();
				return false;
			}
		} else {
			// Release any stale quantization buffer from a previous quantized publish so the
			// non-quantized contract carries no dangling per-chunk bounds.
			p_renderer->free_owned_resource(rd, resource_state.resident_quantization_buffer);
			resource_state.resident_quantization_buffer_size = 0;
		}

		atlas_gaussian_count = atlas_splat_total;
		atlas_max_chunk_count_per_asset = max_chunk_count_per_asset;
		atlas_max_chunk_splats = max_chunk_splats;

		// Cache derived sizes so the next instance-only update can re-publish the contract
		// without re-running the pack loop.
		resource_state.resident_atlas_gaussian_count = atlas_gaussian_count;
		resource_state.resident_dispatch_chunk_count = atlas_max_chunk_count_per_asset;
		resource_state.resident_max_chunk_splats = atlas_max_chunk_splats;
		resource_state.resident_atlas_pack_count++;
		// VRAM-budget LOD clamp telemetry (observable; never a silent drop). Cached so the
		// fast instance-only path re-surfaces the same values; the packed count is
		// resident_atlas_gaussian_count above.
		resource_state.resident_atlas_reduced = subset_plan.reduced;
		resource_state.resident_atlas_source_count = uint32_t(subset_plan.source_count);
		resource_state.resident_atlas_keep_ratio = subset_plan.reduced ? float(subset_plan.keep_ratio) : 1.0f;
		if (subset_plan.reduced) {
			WARN_PRINT_ONCE(vformat("[ResidentContract] Atlas exceeds the resident VRAM budget; thinned by "
					"importance to fit: %d -> %d splats (keep_ratio=%.3f, cap=%d MB). Reduce the asset's splat "
					"count or render it via GaussianSplatWorld3D streaming for full density.",
					int(subset_plan.source_count), int(atlas_gaussian_count), subset_plan.keep_ratio,
					int(subset_plan.cap_bytes >> 20)));
		}
	} else {
		// Atlas unchanged. Reuse the cached size metrics from the last slow-path publish.
		// Atlas storage buffers (resident_atlas_gaussian_buffer etc.) keep their current
		// contents and RIDs.
		atlas_gaussian_count = resource_state.resident_atlas_gaussian_count;
		atlas_max_chunk_count_per_asset = resource_state.resident_dispatch_chunk_count;
		atlas_max_chunk_splats = resource_state.resident_max_chunk_splats;
	}

	const uint32_t instance_count = instances.size();
	GaussianRenderPipeline::InstancePipelineBuffers buffers;
	buffers.atlas_gaussian_buffer = resource_state.resident_atlas_gaussian_buffer;
	buffers.atlas_gaussian_count = atlas_gaussian_count;
	buffers.asset_meta_buffer = resource_state.resident_asset_meta_buffer;
	buffers.chunk_meta_buffer = resource_state.resident_chunk_meta_buffer;
	buffers.asset_chunk_index_buffer = resource_state.resident_asset_chunk_index_buffer;
	// Derive from the resource-state buffer so the cached instance-only re-publish path
	// (which skips the pack loop) restores the quantized contract too: a quantized publish
	// leaves resident_quantization_buffer valid; a non-quantized one frees it.
	buffers.quantization_required = resource_state.resident_quantization_buffer.is_valid();
	buffers.quantization_buffer = resource_state.resident_quantization_buffer;
	buffers.dispatch_chunk_count = MAX<uint32_t>(1u, atlas_max_chunk_count_per_asset);
	buffers.max_chunk_splats = MAX<uint32_t>(1u, atlas_max_chunk_splats);

	uint64_t max_visible_splats_u64 = atlas_gaussian_count;
	const int configured_max_splats = p_renderer->get_performance_settings().max_splats;
	if (configured_max_splats > 0) {
		max_visible_splats_u64 = MIN<uint64_t>(max_visible_splats_u64, uint64_t(configured_max_splats));
	}
	const uint64_t instance_requirement = uint64_t(instance_count) * uint64_t(buffers.dispatch_chunk_count) *
			uint64_t(buffers.max_chunk_splats);
	max_visible_splats_u64 = MAX<uint64_t>(max_visible_splats_u64, instance_requirement);
	const uint64_t sort_cap = g_gpu_sorting_config.max_sort_elements > 0
			? uint64_t(g_gpu_sorting_config.max_sort_elements)
			: uint64_t(UINT32_MAX);
	buffers.max_visible_splats = uint32_t(MIN<uint64_t>(max_visible_splats_u64, sort_cap));
	uint64_t max_visible_chunks_u64 = uint64_t(instance_count) * uint64_t(buffers.dispatch_chunk_count);
	buffers.max_visible_chunks = uint32_t(MIN<uint64_t>(max_visible_chunks_u64,
			uint64_t(MAX<uint32_t>(1u, buffers.max_visible_splats))));

	auto ensure_owner = [&](RID &r_buffer, uint32_t *r_capacity) {
		if (r_buffer.is_valid() && p_renderer->get_resource_owner(r_buffer, rd) != rd) {
			p_renderer->free_owned_resource(rd, r_buffer);
			if (r_capacity != nullptr) {
				*r_capacity = 0;
			}
		}
	};
	ensure_owner(resource_state.instance_visible_chunk_buffer, &resource_state.instance_visible_chunk_capacity);
	ensure_owner(resource_state.instance_splat_ref_buffer, &resource_state.instance_splat_ref_capacity);
	ensure_owner(resource_state.instance_counter_buffer, nullptr);
	ensure_owner(resource_state.instance_chunk_dispatch_buffer, nullptr);
	ensure_owner(resource_state.instance_indirect_count_buffer, nullptr);
	ensure_owner(resource_state.instance_count_buffer, nullptr);

	if (!resource_state.instance_visible_chunk_buffer.is_valid() ||
			resource_state.instance_visible_chunk_capacity < buffers.max_visible_chunks) {
		p_renderer->free_owned_resource(rd, resource_state.instance_visible_chunk_buffer);
		const uint32_t buffer_size = MAX<uint32_t>(1u, buffers.max_visible_chunks) * sizeof(VisibleChunkRefGPU);
		resource_state.instance_visible_chunk_buffer = rd->storage_buffer_create(buffer_size);
		if (!resource_state.instance_visible_chunk_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_visible_chunk_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_visible_chunk_buffer, "GS_InstanceVisibleChunks");
		p_renderer->track_resource_owner(resource_state.instance_visible_chunk_buffer, rd);
		resource_state.instance_visible_chunk_capacity = MAX<uint32_t>(1u, buffers.max_visible_chunks);
	}
	buffers.visible_chunk_buffer = resource_state.instance_visible_chunk_buffer;

	if (!resource_state.instance_splat_ref_buffer.is_valid() ||
			resource_state.instance_splat_ref_capacity < buffers.max_visible_splats) {
		p_renderer->free_owned_resource(rd, resource_state.instance_splat_ref_buffer);
		const uint32_t buffer_size = MAX<uint32_t>(1u, buffers.max_visible_splats) * sizeof(SplatRefGPU);
		resource_state.instance_splat_ref_buffer = rd->storage_buffer_create(buffer_size);
		if (!resource_state.instance_splat_ref_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_splat_ref_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_splat_ref_buffer, "GS_InstanceSplatRefs");
		p_renderer->track_resource_owner(resource_state.instance_splat_ref_buffer, rd);
		resource_state.instance_splat_ref_capacity = MAX<uint32_t>(1u, buffers.max_visible_splats);
	}
	buffers.splat_ref_buffer = resource_state.instance_splat_ref_buffer;

	if (!resource_state.instance_counter_buffer.is_valid()) {
		// 3 uints: [0]=visible_chunk_count, [1]=overflowed_chunks, [2]=frustum_culled_chunks
		// (M0 telemetry; written only by frustum_cull.glsl, survives the Stage-B counter reuse).
		resource_state.instance_counter_buffer = rd->storage_buffer_create(sizeof(uint32_t) * 3);
		if (!resource_state.instance_counter_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_counter_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_counter_buffer, "GS_InstanceCounters");
		p_renderer->track_resource_owner(resource_state.instance_counter_buffer, rd);
	}
	buffers.counter_buffer = resource_state.instance_counter_buffer;

	if (!resource_state.instance_chunk_dispatch_buffer.is_valid()) {
		resource_state.instance_chunk_dispatch_buffer = rd->storage_buffer_create(
				sizeof(uint32_t) * 3, Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		if (!resource_state.instance_chunk_dispatch_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_chunk_dispatch_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_chunk_dispatch_buffer, "GS_InstanceChunkDispatch");
		p_renderer->track_resource_owner(resource_state.instance_chunk_dispatch_buffer, rd);
	}
	buffers.chunk_dispatch_buffer = resource_state.instance_chunk_dispatch_buffer;

	if (!resource_state.instance_indirect_count_buffer.is_valid()) {
		resource_state.instance_indirect_count_buffer = rd->storage_buffer_create(
				sizeof(GaussianSplatting::IndirectDispatchLayout), Vector<uint8_t>(),
				RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		if (!resource_state.instance_indirect_count_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_indirect_count_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_indirect_count_buffer, "GS_InstanceIndirectCount");
		p_renderer->track_resource_owner(resource_state.instance_indirect_count_buffer, rd);
	}
	buffers.indirect_count_buffer = resource_state.instance_indirect_count_buffer;

	if (!resource_state.instance_count_buffer.is_valid()) {
		// Zero-init: the instance-count clamp accumulates a STICKY overflow_flag
		// (C4b/G4, Channel B), so the buffer must start clean and never accumulate
		// onto uninitialized memory.
		Vector<uint8_t> instance_count_init;
		instance_count_init.resize(sizeof(GaussianSplatting::IndirectDispatchLayout));
		instance_count_init.fill(0);
		resource_state.instance_count_buffer = rd->storage_buffer_create(
				instance_count_init.size(), instance_count_init);
		if (!resource_state.instance_count_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_instance_count_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_count_buffer, "GS_InstanceCount");
		p_renderer->track_resource_owner(resource_state.instance_count_buffer, rd);
	}
	buffers.instance_count_buffer = resource_state.instance_count_buffer;

	// Mirror the render_streaming_orchestrator pattern: forward the persistent
	// per-renderer instance_grading_buffer RID through the local `buffers`
	// snapshot so it survives publish_instance_pipeline_contract() below. The
	// subsequent update_instance_grading_buffer() call (~line 687) refreshes
	// the GPU contents; this line ensures the RID itself lands in
	// instance_pipeline_buffers even on paths that hit clear-then-republish
	// sequences, so first_raster_violation() doesn't read a stale RID() in
	// the gap before update_instance_grading_buffer() runs.
	buffers.instance_grading_buffer = resource_state.instance_grading_buffer;

	Ref<GPUSortingPipeline> sorting_pipeline = p_renderer->get_subsystem_state().sorting_pipeline;
	if (sorting_pipeline.is_null()) {
		if (r_reason) {
			*r_reason = "resident_sorting_pipeline_unavailable";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}
	const uint32_t required_sort_capacity = MAX<uint32_t>(1u, buffers.max_visible_splats);
	if (required_sort_capacity > sorting_pipeline->get_max_elements()) {
		sorting_pipeline->rebuild_sorter(required_sort_capacity);
	}
	sorting_pipeline->ensure_buffers(required_sort_capacity);
	const SortBufferHandles sort_handles = sorting_pipeline->get_buffer_handles();
	if (!sort_handles.valid) {
		if (r_reason) {
			*r_reason = "resident_sort_buffers_unavailable";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}
	buffers.sort_key_buffer = sort_handles.keys_buffer;
	buffers.sort_value_buffer = sort_handles.indices_buffer;
	if (sort_handles.capacity > 0 && buffers.max_visible_splats > sort_handles.capacity) {
		buffers.max_visible_splats = sort_handles.capacity;
		buffers.max_visible_chunks = MIN<uint32_t>(buffers.max_visible_chunks, buffers.max_visible_splats);
	}

	GaussianRenderPipeline::PublishedInstanceAssetRemap remap;
	remap.asset_to_dense_id.insert(kPrimaryResidentAssetId, 0u);
	for (const ResidentAssetDescriptor &asset : assets) {
		remap.asset_to_dense_id.insert(asset.submission_asset_id, asset.dense_asset_id);
	}
	remap.generation = source_generation == 0 ? 1u : source_generation;
	remap.valid = true;

	p_renderer->publish_instance_pipeline_contract(buffers, remap,
			GaussianRenderPipeline::InstanceBackendPolicy::RESIDENT, source_generation, "atlas_emulation");
	resource_state.instance_pipeline_contract_generation = source_generation;
	resource_state.instance_pipeline_atlas_generation = atlas_generation;
	resource_state.instance_pipeline_content_generation = source_generation;
	resource_state.instance_pipeline_contract_fingerprint = 0;
	resource_state.instance_pipeline_upload_generation = source_generation;
	resource_state.instance_pipeline_upload_fingerprint = 0;

	if (!p_renderer->update_instance_buffer(instances, remap, &instance_submission_asset_ids)) {
		if (r_reason) {
			*r_reason = "resident_instance_upload_failed";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

	// Upload per-instance color grading. Walks the same director instance list so rows
	// line up 1:1 with SplatRefGPU.instance_id. Must run whenever instance_buffer is
	// re-uploaded because the instance list may have changed rows.
	{
		LocalVector<InstanceGradingGPU> gradings;
		if (director != nullptr) {
			director->build_instance_grading_buffer_for_renderer(p_renderer, gradings,
					p_renderer->is_shadow_instance_filter_enabled());
		}
		if (gradings.is_empty() && !instances.is_empty()) {
			// If director records are absent but the resident direct-data path
			// injected a bootstrap instance above, seed grading rows from the
			// renderer-wide color_grading so direct-data / worldless renderers
			// keep their grading on this path instead of being forced to neutral.
			const Ref<ColorGradingResource> renderer_default = p_renderer->get_color_grading();
			gradings.resize(instances.size());
			for (uint32_t i = 0; i < gradings.size(); ++i) {
				GaussianSplatSceneDirector::fill_instance_grading_entry(renderer_default, gradings[i]);
			}
		}
		// Contract: the grading buffer MUST have the same row count as the
		// instance buffer. Shader indexing (instance_buffer.instances[splat_ref.instance_id]
		// and instance_grading_buffer.gradings[splat_ref.instance_id]) relies on 1:1
		// parity. If the filter logic ever drifts between the two director build
		// steps, early-fail here instead of uploading a short buffer the shader
		// would then read past end-of-array.
		if (gradings.size() != instances.size()) {
			ERR_PRINT_ONCE(vformat(
					"[ResidentContract] grading buffer row count (%d) does not match instance buffer row count (%d). "
					"build_instance_grading_buffer_for_renderer and build_instance_buffer_for_renderer must apply identical filters.",
					int(gradings.size()), int(instances.size())));
			if (r_reason) {
				*r_reason = "resident_grading_instance_row_mismatch";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		if (!p_renderer->update_instance_grading_buffer(gradings)) {
			if (r_reason) {
				*r_reason = "resident_grading_upload_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
	}

	const GaussianRenderPipeline::InstancePipelineBuffers &published_buffers = p_renderer->get_instance_pipeline_buffers();
	InvariantViolationReason violation_reason = InvariantViolationReason::NONE;
	if (!GaussianSplatting::InstancePipelineContract::has_atlas_buffers(published_buffers)) {
		violation_reason = GaussianSplatting::InstancePipelineContract::first_atlas_violation(published_buffers);
	} else if (!GaussianSplatting::InstancePipelineContract::has_cull_buffers(published_buffers)) {
		violation_reason = GaussianSplatting::InstancePipelineContract::first_cull_violation(published_buffers);
	} else if (!GaussianSplatting::InstancePipelineContract::has_sort_buffers(published_buffers)) {
		violation_reason = GaussianSplatting::InstancePipelineContract::first_sort_violation(published_buffers);
	} else if (!GaussianSplatting::InstancePipelineContract::has_raster_buffers(published_buffers)) {
		violation_reason = GaussianSplatting::InstancePipelineContract::first_raster_violation(published_buffers);
	}
	if (violation_reason != InvariantViolationReason::NONE) {
		if (r_reason) {
			*r_reason = GaussianSplatting::InstancePipelineContract::get_violation_reason_name(violation_reason);
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

	return true;
}

} // namespace ResidentInstanceContractPublisher

namespace {
using HlodBundle = GaussianRenderFacadeState::HlodBundle;

// `p_atlas_records` != 0 sizes the atlas for a streamed bundle (node streaming, package D): the atlas holds that
// many splat records, not the whole tree. 0 keeps the whole-tree layout. `p_rows` instance rows (row 0 is the world);
// the ledger itself is gs_hlod_bundle::bundle_bytes (CPU-tested; p_rows == 1 is the single-row ledger exactly). PR1b:
// `p_region` adds the packed node assets (an empty region adds nothing).
uint64_t hlod_bundle_bytes(const GaussianSplatHlodRuntimeData &p_data, bool p_relative, bool p_quantized, uint32_t p_capacity,
        uint32_t p_rows, const gs_hlod_bundle::InstanceRegion &p_region, uint32_t p_atlas_records = 0) {
    const uint64_t records = p_atlas_records ? uint64_t(p_atlas_records) : uint64_t(p_data.leaf_count) + (p_relative ? p_data.parent_count : 0);
    return gs_hlod_bundle::bundle_bytes(p_data.nodes.size(), records, p_relative, p_quantized, p_capacity, p_rows, p_region);
}

// PR1b: the node assets of an HLOD bundle, packed once on the CPU (region-local addresses) and placed after the world by
// hlod_build_bundle(). Positions are asset-local in both bundles (the relative frame takes T - camera per draw, the
// canonical frame the row's T), so the same records serve the canonical and the relative bundle.
struct HlodInstancePayload {
    gs_hlod_bundle::InstanceRegion region;
    Vector<PackedGaussian> raw;
    Vector<PackedGaussianQuantized> quantized;
    LocalVector<ChunkMetaGPU> chunk_meta; // atlas_base region-local; quant_base already node_count + chunk.
    LocalVector<ChunkQuantizationGPU> quantization; // start_index region-local.
    LocalVector<AssetMetaGPU> asset_meta; // One per region asset; chunk_index_base region-local.
};

// Packs every distinct node asset the rows name, whole, in ascending asset id order, as the resident atlas packs a
// node asset (contiguous chunks of GaussianStreamingSystem::CHUNK_SIZE, raw or per-chunk quantized). No thinning: an
// object is drawn whole or refused (the budget rule), never partially. False with the reason on any failure.
bool hlod_pack_instance_assets(const LocalVector<InstanceAssetRegistration> &p_registered, const gs_hlod_bundle::Rows &p_rows,
        uint32_t p_node_count, bool p_quantized, HlodInstancePayload &r_payload, String &r_reason) {
    r_payload = HlodInstancePayload();
    LocalVector<uint64_t> ids;
    for (uint32_t i = 0; i < p_rows.asset_ids.size(); ++i) {
        if (p_rows.asset_ids[i] != gs_hlod_bundle::WORLD_ASSET_ID) { ids.push_back(p_rows.asset_ids[i]); }
    }
    std::sort(ids.ptr(), ids.ptr() + ids.size());
    const uint32_t position_bits = MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX);
    const uint32_t scale_bits = MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX);
    for (uint32_t i = 0; i < ids.size(); ++i) {
        if (i > 0 && ids[i] == ids[i - 1]) { continue; }
        Ref<GaussianData> data;
        for (const InstanceAssetRegistration &entry : p_registered) {
            if (entry.asset_id == ids[i]) { data = entry.data; break; }
        }
        if (data.is_null() || data->get_count() <= 0) { r_reason = "hlod_instance_asset_unavailable"; return false; }
        ResidentAssetDescriptor descriptor_asset;
        descriptor_asset.submission_asset_id = ids[i];
        descriptor_asset.dense_asset_id = r_payload.region.assets.size() + 1u;
        descriptor_asset.data = data;
        LocalVector<ResidentChunkDescriptor> chunks;
        if (!_append_chunk_descriptors_for_asset(descriptor_asset, chunks) || chunks.is_empty()) {
            r_reason = "hlod_instance_chunk_descriptor_failed"; return false;
        }
        gs_hlod_bundle::InstanceAsset asset;
        asset.asset_id = ids[i];
        asset.first_chunk = r_payload.region.chunk_count;
        asset.first_record = r_payload.region.records;
        const AABB bounds = data->get_aabb();
        asset.bounds_center = bounds.get_center();
        asset.bounds_radius = (bounds.size * 0.5f).length();
        AssetMetaGPU meta = {};
        meta.lod_count = 1;
        meta.sh_degree = data->get_sh_degree();
        meta.flags = gs_pack_asset_gpu_flags(data->get_2d_mode(), _resolve_data_dc_encoding(data));
        for (int a = 0; a < 3; ++a) { meta.bounds_center_local[a] = asset.bounds_center[a]; }
        meta.bounds_radius_local = asset.bounds_radius;
        meta.chunk_index_base = asset.first_chunk;
        for (const ResidentChunkDescriptor &chunk : chunks) {
            LocalVector<Gaussian> gaussians;
            LocalVector<Vector3> sh;
            uint32_t first_order = 0, high_order = 0;
            const bool captured = chunk.source_index_remapped
                    ? data->capture_indexed_chunk_snapshot(chunk.source_indices.ptr(), chunk.count, gaussians, sh, first_order, high_order)
                    : data->capture_chunk_snapshot(chunk.start_idx, chunk.count, gaussians, sh, first_order, high_order);
            if (!captured || gaussians.size() != chunk.count || chunk.count == 0) { r_reason = "hlod_instance_chunk_snapshot_failed"; return false; }
            const uint32_t chunk_id = p_node_count + r_payload.region.chunk_count; // Index into the bundle's chunk tables.
            if (p_quantized && chunk_id > 0xFFFFu) {
                // PackedGaussianQuantized carries a uint16 chunk id (the resident atlas refuses the same way).
                r_reason = "hlod_instance_quantization_chunk_overflow"; return false;
            }
            const Vector3 *sh_coeffs = sh.is_empty() ? nullptr : sh.ptr();
            gs_warn_sh_layout_truncation_once(**data, first_order, high_order, p_quantized);
            SHCompressionMetrics metrics;
            const uint64_t base = r_payload.region.records;
            ChunkMetaGPU chunk_meta = {};
            if (p_quantized) {
                ChunkQuantizationInfo quant;
                quant.compute_from_gaussians(gaussians, 0, chunk.count, position_bits, scale_bits, /*quantize_scale=*/true);
                if (!gs_resize_or_fail(r_payload.quantized, int64_t(base + chunk.count), "HLOD instance packed quantized")) {
                    r_reason = "hlod_instance_payload_allocation_failed"; return false;
                }
                pack_gaussians_range_quantized(gaussians, 0, chunk.count, quant, uint16_t(chunk_id),
                        r_payload.quantized.ptrw() + base, metrics, sh_coeffs, first_order, high_order);
                ChunkQuantizationGPU q = {};
                for (int a = 0; a < 3; ++a) {
                    q.position_min[a] = quant.position_min[a];
                    q.position_range[a] = quant.position_range[a];
                    q.scale_min[a] = quant.scale_min[a];
                    q.scale_range[a] = quant.scale_range[a];
                }
                q.position_bits = quant.position_bits;
                q.scale_bits = quant.scales_quantized ? quant.scale_bits : 0u;
                q.start_index = uint32_t(base);
                q.count = chunk.count;
                r_payload.quantization.push_back(q);
                chunk_meta.quant_base = chunk_id;
                chunk_meta.quant_count = 1u;
            } else {
                Vector<PackedGaussian> packed;
                if (!pack_gaussians_range(gaussians, 0, chunk.count, packed, metrics, sh_coeffs, first_order, high_order) ||
                        uint32_t(packed.size()) != chunk.count ||
                        !gs_resize_or_fail(r_payload.raw, int64_t(base + chunk.count), "HLOD instance packed raw")) {
                    r_reason = "hlod_instance_payload_allocation_failed"; return false;
                }
                memcpy(r_payload.raw.ptrw() + base, packed.ptr(), size_t(chunk.count) * sizeof(PackedGaussian));
            }
            chunk_meta.atlas_base = uint32_t(base);
            chunk_meta.splat_count = chunk.count;
            const Vector3 chunk_center = chunk.bounds.get_center();
            for (int a = 0; a < 3; ++a) { chunk_meta.bounds_center_local[a] = chunk_center[a]; }
            chunk_meta.bounds_radius_local = (chunk.bounds.size * 0.5f).length();
            chunk_meta.asset_id = descriptor_asset.dense_asset_id;
            chunk_meta.lod_level = 0;
            chunk_meta.flags = meta.flags;
            chunk_meta.sh_limit = CLAMP(data->get_sh_degree(), 0u, 4u);
            r_payload.chunk_meta.push_back(chunk_meta);
            // The record-cost footprint, with the HLOD bake's per-splat rule (36 * s1 * s2, the two largest axes).
            for (const Gaussian &g : gaussians) {
                double s[3] = { std::fabs(double(g.scale.x)), std::fabs(double(g.scale.y)), std::fabs(double(g.scale.z)) };
                std::sort(s, s + 3);
                asset.footprint += 36.0 * s[1] * s[2];
            }
            asset.max_chunk_splats = MAX(asset.max_chunk_splats, chunk.count);
            ++asset.chunk_count;
            asset.splat_count += chunk.count;
            ++r_payload.region.chunk_count;
            r_payload.region.records += chunk.count;
        }
        if (!std::isfinite(asset.footprint)) { r_reason = "hlod_instance_footprint_invalid"; return false; }
        meta.chunk_index_count = asset.chunk_count;
        meta.lod_ranges[0].base = asset.first_chunk;
        meta.lod_ranges[0].count = asset.chunk_count;
        r_payload.asset_meta.push_back(meta);
        r_payload.region.max_chunk_splats = MAX(r_payload.region.max_chunk_splats, asset.max_chunk_splats);
        r_payload.region.assets.push_back(asset);
    }
    if (!gs_hlod_bundle::tally_rows(p_rows, r_payload.region)) { r_reason = "hlod_bundle_row_asset_not_packed"; return false; }
    return true;
}

bool hlod_create_buffer(GaussianSplatRenderer *p_renderer, RenderingDevice *p_rd, HlodBundle &r_bundle,
        RID &r_rid, uint64_t p_bytes, const void *p_data = nullptr, bool p_indirect = false) {
    if (p_bytes == 0 || p_bytes > UINT32_MAX || r_bundle.owned_count >= 14) { return false; }
#ifdef TESTS_ENABLED
    // Fail after one real allocation, so rollback must release a partial candidate.
    if (r_bundle.owned_count == 1 && p_renderer->test_hlod_publication_refuses(4)) { return false; }
#endif
    Vector<uint8_t> initial;
    if (!gs_resize_or_fail(initial, p_bytes, "HLOD candidate upload")) { return false; }
    if (p_data) { memcpy(initial.ptrw(), p_data, p_bytes); } else { initial.fill(0); }
    r_rid = p_rd->storage_buffer_create(uint32_t(p_bytes), initial,
            p_indirect ? RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT : 0);
    if (!r_rid.is_valid()) { return false; }
    p_renderer->track_resource_owner(r_rid, p_rd);
    r_bundle.owned[r_bundle.owned_count++] = r_rid;
    r_bundle.bytes += p_bytes;
    return true;
}

bool hlod_build_bundle(GaussianSplatRenderer *p_renderer, RenderingDevice *p_rd,
        const GaussianSplatHlodRuntimeData &p_data, bool p_relative, bool p_quantized,
        uint32_t p_capacity, uint32_t p_position_bits, uint32_t p_scale_bits, const gs_hlod_bundle::Rows &p_rows,
        const HlodInstancePayload &p_instances, HlodBundle &r_bundle, bool p_streaming = false, uint32_t p_atlas_records = 0) {
    // Package F1: a whole-tree bundle packs payload by base ranges; an edited snapshot is served by the streamer only.
    if (!p_streaming && p_data.has_edits()) { return false; }
    // Instance rows: row 0 is the world, rows 1..N name node assets packed in the instance region (PR1b).
    const gs_hlod_bundle::InstanceRegion &region = p_instances.region;
    if (gs_hlod_bundle::rows_refusal(p_rows, uint64_t(p_data.nodes.size()), region) != nullptr) { return false; }
    if ((p_quantized ? uint64_t(p_instances.quantized.size()) : uint64_t(p_instances.raw.size())) != region.records ||
            p_instances.chunk_meta.size() != region.chunk_count || p_instances.asset_meta.size() != region.assets.size() ||
            (p_quantized && p_instances.quantization.size() != region.chunk_count)) { return false; }
    const uint32_t row_count = p_rows.count();
    LocalVector<InstanceDataGPU> packed_rows;
    gs_hlod_bundle::pack_instance_rows(p_rows, region, packed_rows);
    const uint32_t visible_chunks = uint32_t(gs_hlod_bundle::visible_chunk_capacity(p_data.nodes.size(), p_relative, region));
    auto &b = r_bundle.buffers;
    const uint32_t node_count = p_data.nodes.size();
    const uint32_t records = p_streaming ? p_atlas_records : p_data.leaf_count + (p_relative ? p_data.parent_count : 0);
    // The instance region follows the world: chunk tables [node_count, +chunk_count), atlas [records, +region.records).
    const uint32_t table_count = node_count + region.chunk_count;
    const uint64_t total_records = uint64_t(records) + region.records;
    if (total_records > UINT32_MAX) { return false; }
    Vector<PackedGaussian> raw;
    Vector<PackedGaussianQuantized> quantized;
    Vector<ChunkMetaGPU> metadata;
    Vector<AssetChunkIndexGPU> indices;
    Vector<ChunkQuantizationGPU> quantization;
    // A streamed bundle uploads no world payload up front (the streamer pages it in); its instance tail is written after
    // the buffer exists. A whole-tree bundle uploads world and instance records in one initial copy.
    const uint32_t initial_records = p_streaming ? 0u : uint32_t(total_records);
    if (!gs_resize_or_fail(metadata, table_count, "HLOD candidate metadata") ||
            !gs_resize_or_fail(indices, table_count, "HLOD candidate indices") ||
            !gs_resize_or_fail(r_bundle.draw_entries, p_relative ? visible_chunks : 0, "HLOD candidate draw list") || // Canonical bundle: the GPU culler writes its list; no CPU copy (DrawnNodeGPU is 4x VisibleChunkRefGPU).
            (p_quantized ? (!gs_resize_or_fail(quantized, initial_records, "HLOD candidate packed quantized") ||
                                   !gs_resize_or_fail(quantization, table_count, "HLOD candidate quantization")) :
                           !gs_resize_or_fail(raw, initial_records, "HLOD candidate packed raw"))) {
        return false;
    }
    metadata.fill(ChunkMetaGPU{});
    if (p_quantized) { quantization.fill(ChunkQuantizationGPU{}); }
    uint32_t chunk_count = 0;
    SHCompressionMetrics metrics;
    gs_hlod_pack::Params pack_params;
    pack_params.relative = p_relative;
    pack_params.quantized = p_quantized;
    pack_params.position_bits = p_position_bits;
    pack_params.scale_bits = p_scale_bits;
    for (uint32_t n = 0; n < node_count; ++n) {
        const auto &node = p_data.nodes[n];
        if (!p_relative && !node.topology.is_leaf()) { continue; }
        const uint32_t first = uint32_t(node.topology.payload_first);
        indices.write[chunk_count++].chunk_id = n;
        b.max_chunk_splats = MAX(b.max_chunk_splats, node.topology.payload_count);
        // A streamed bundle starts with an all-zero node table (splat_count 0): a node that is not resident
        // is invisible to the GPU even if a stale draw record named it. The node streamer writes the metadata
        // together with the payload, when the node becomes resident-ready.
        if (p_streaming) { continue; }
        gs_hlod_pack::Output out;
        out.raw = p_quantized ? nullptr : raw.ptrw() + first;
        out.quantized = p_quantized ? quantized.ptrw() + first : nullptr;
        out.meta = &metadata.write[n];
        out.quant = p_quantized ? &quantization.write[n] : nullptr;
        out.metrics = &metrics;
        if (!gs_hlod_pack::pack_node(p_data, n, pack_params, first, out)) { return false; }
    }
    LocalVector<AssetMetaGPU> assets;
    assets.resize(1u + region.assets.size());
    AssetMetaGPU &asset = assets[0];
    asset = AssetMetaGPU{};
    asset.lod_count = 1;
    asset.sh_degree = p_data.sh_degree;
    asset.flags = gs_pack_asset_gpu_flags(p_data.mode_2d, GaussianDCEncoding(p_data.nodes[indices[0].chunk_id].dc_encoding));
    asset.chunk_index_count = chunk_count;
    asset.quant_chunk_count = p_quantized ? node_count : 0;
    asset.lod_ranges[0].count = chunk_count;
    for (int a = 0; a < 3; ++a) { asset.bounds_center_local[a] = p_data.nodes[0].bounds_center[a]; }
    asset.bounds_radius_local = p_data.nodes[0].topology.radius;
    // PR1b: the instance region, after the world. Its chunk tables sit at [node_count, table_count) and its records after
    // the world's `records`, so the world's tables and atlas are exactly what they were without instances.
    for (uint32_t k = 0; k < region.chunk_count; ++k) {
        ChunkMetaGPU meta = p_instances.chunk_meta[k];
        meta.atlas_base += records;
        metadata.write[node_count + k] = meta;
        indices.write[node_count + k].chunk_id = node_count + k;
        if (p_quantized) {
            ChunkQuantizationGPU q = p_instances.quantization[k];
            q.start_index += records;
            quantization.write[node_count + k] = q;
        }
    }
    for (uint32_t a = 0; a < region.assets.size(); ++a) {
        AssetMetaGPU meta = p_instances.asset_meta[a];
        meta.chunk_index_base += node_count;
        meta.lod_ranges[0].base += node_count;
        meta.quant_chunk_count = p_quantized ? meta.chunk_index_count : 0;
        assets[1u + a] = meta;
    }
    if (!p_streaming && region.records != 0) {
        if (p_quantized) {
            memcpy(quantized.ptrw() + records, p_instances.quantized.ptr(), size_t(region.records) * sizeof(PackedGaussianQuantized));
        } else {
            memcpy(raw.ptrw() + records, p_instances.raw.ptr(), size_t(region.records) * sizeof(PackedGaussian));
        }
    }
    b.max_chunk_splats = MAX(b.max_chunk_splats, region.max_chunk_splats);
    uint32_t max_asset_chunks = chunk_count;
    for (const gs_hlod_bundle::InstanceAsset &instance_asset : region.assets) { max_asset_chunks = MAX(max_asset_chunks, instance_asset.chunk_count); }
    auto create = [&](RID &r_rid, uint64_t p_bytes, const void *p_initial = nullptr, bool p_indirect = false) {
        return hlod_create_buffer(p_renderer, p_rd, r_bundle, r_rid, p_bytes, p_initial, p_indirect);
    };
    const uint64_t record_stride = p_quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
    if (!create(b.atlas_gaussian_buffer, total_records * record_stride,
                p_streaming ? nullptr : (p_quantized ? static_cast<const void *>(quantized.ptr()) : raw.ptr())) ||
            !create(b.asset_meta_buffer, uint64_t(assets.size()) * sizeof(AssetMetaGPU), assets.ptr()) ||
            !create(b.asset_chunk_index_buffer, uint64_t(table_count) * sizeof(AssetChunkIndexGPU), indices.ptr()) ||
            !create(b.chunk_meta_buffer, uint64_t(table_count) * sizeof(ChunkMetaGPU), metadata.ptr()) ||
            (p_quantized && !create(b.quantization_buffer, uint64_t(table_count) * sizeof(ChunkQuantizationGPU), quantization.ptr())) ||
            !create(b.instance_buffer, uint64_t(row_count) * sizeof(InstanceDataGPU), packed_rows.ptr()) ||
            !create(b.instance_grading_buffer, uint64_t(row_count) * sizeof(InstanceGradingGPU), p_rows.grading.ptr()) ||
            !create(b.visible_chunk_buffer, uint64_t(visible_chunks) * (p_relative ? sizeof(DrawnNodeGPU) : sizeof(VisibleChunkRefGPU))) ||
            !create(b.splat_ref_buffer, uint64_t(p_capacity) * sizeof(SplatRefGPU)) ||
            (p_relative && !create(b.hlod_draw_entry_buffer, uint64_t(p_capacity) * sizeof(uint32_t))) ||
            !create(b.counter_buffer, 3 * sizeof(uint32_t)) ||
            !create(b.chunk_dispatch_buffer, 3 * sizeof(uint32_t), nullptr, true) ||
            !create(b.indirect_count_buffer, sizeof(GaussianSplatting::IndirectDispatchLayout), nullptr, true) ||
            !create(b.instance_count_buffer, sizeof(GaussianSplatting::IndirectDispatchLayout))) {
        return false;
    }
    if (p_streaming && region.records != 0) {
        // The streamed atlas's world pages start empty; the instance tail is resident from the start (the streamer only
        // ever addresses its pages and the world's node-indexed tables, so the two regions are disjoint).
        const void *tail = p_quantized ? static_cast<const void *>(p_instances.quantized.ptr()) : static_cast<const void *>(p_instances.raw.ptr());
        if (p_rd->buffer_update(b.atlas_gaussian_buffer, uint32_t(uint64_t(records) * record_stride), uint32_t(region.records * record_stride), tail) != OK) {
            return false;
        }
    }
    b.quantization_required = p_quantized;
    b.atlas_gaussian_count = uint32_t(total_records);
    b.instance_count = row_count;
    // Canonical: the GPU culler's per-asset chunk dispatch (the largest asset). Relative: the candidate count it reports.
    b.dispatch_chunk_count = p_relative ? chunk_count + region.chunk_count : max_asset_chunks;
    b.max_visible_chunks = visible_chunks;
    b.max_visible_splats = p_capacity;
    // Both bundles belong to the resident route. Preserve its per-splat
    // frustum/size/distance culling; the streamed-world exception is separate.
    // Relative center reconstruction and draw-list dispatch use hlod_relative.
    b.world_submission_active = false;
    b.hlod_relative = p_relative;
    b.hlod_source_generation = p_data.get_instance_id();
    r_bundle.generation = p_data.get_instance_id();
    r_bundle.device_id = p_rd->get_device_instance_id();
    r_bundle.quantized = p_quantized;
    return r_bundle.bytes == hlod_bundle_bytes(p_data, p_relative, p_quantized, p_capacity, row_count, region, p_streaming ? p_atlas_records : 0);
}
}

void GaussianSplatRenderer::release_hlod_bundle(GaussianRenderFacadeState::HlodBundle &r_bundle) {
    // Until teardown, retain a conservative charge: RD deferred destruction has no
    // renderer-local completion fence. Never infer disposal from a frame count.
    get_resource_state().hlod_retired_bytes += r_bundle.bytes;
    for (uint32_t i = 0; i < r_bundle.owned_count; ++i) {
        free_owned_resource(get_device_state().rd, r_bundle.owned[i]);
    }
    r_bundle = GaussianRenderFacadeState::HlodBundle();
}

void GaussianSplatRenderer::release_hlod_resources() {
    auto &state = get_resource_state();
#ifdef TESTS_ENABLED
    state.hlod_test_sort_count_buffer = RID();
#endif
    if (get_instance_pipeline_buffers().atlas_gaussian_buffer == state.hlod_canonical.buffers.atlas_gaussian_buffer ||
            get_instance_pipeline_buffers().atlas_gaussian_buffer == state.hlod_relative.buffers.atlas_gaussian_buffer) {
        clear_instance_pipeline_buffers();
    }
    release_hlod_bundle(state.hlod_canonical);
    release_hlod_bundle(state.hlod_relative);
    state.device_vram_ledger.clear_hlod_reports();
    if (state.hlod_streamer.is_valid()) {
        state.hlod_streamer->shutdown(); // Waits for in-flight packers; nothing references the freed buffers.
        state.hlod_streamer.unref();
    }
    state.hlod_streaming = false;
    state.hlod_stream_atlas_records = 0;
    state.hlod_stream_ready_frames = state.hlod_stream_loading_frames = state.hlod_stream_first_ready_frame = 0;
    state.hlod_stream_frames = 0;
    state.hlod_prepared_snapshot.unref();
    state.hlod_active = false;
    state.hlod_frame_active = false;
    state.hlod_selected_nodes = state.hlod_selected_parents = state.hlod_visited_nodes = 0;
    state.hlod_selected_splats = 0;
    state.hlod_instance_generation = state.hlod_instance_asset_generation = state.hlod_grading_generation = state.hlod_prepared_owner_key = 0;
    state.hlod_instance_rows = 0;
    state.hlod_shadow_caster_record = gs_shadow_caster::FrameRecord(); // The buffers it names are gone.
    state.hlod_instance_region = gs_hlod_bundle::InstanceRegion();
    state.hlod_instance_row_data = gs_hlod_bundle::Rows();
    state.hlod_instance_row_keys.clear();
    state.hlod_relative_capacity = state.hlod_canonical_capacity = 0;
    state.hlod_remap_asset_generation = 0;
    state.hlod_world_draw_records = state.hlod_instance_draw_records = 0;
    state.clear_hlod_level_drawn();
    state.hlod_instance_draws.clear();
    frame_context_manager.clear_hlod_views();
}

bool GaussianSplatRenderer::is_hlod_prepared_contract_current() const {
    const auto &state = get_resource_state();
    const Ref<GaussianSplatHlodRuntimeData> input = state.hlod_prepared_snapshot;
    RenderingDevice *rd = get_device_state().rd;
    auto *director = GaussianSplatSceneDirector::get_singleton();
    if (input.is_null() || input != state.hlod_snapshot || !input->is_current() ||
            input->source_data != get_scene_state().gaussian_data || !rd || !director ||
            !gs::settings::get_hlod_runtime_cut_enabled(ProjectSettings::get_singleton()) ||
            !director->has_hlod_world_submission(this, state.hlod_owner_key) ||
            state.hlod_prepared_owner_key != state.hlod_owner_key || is_shadow_instance_filter_enabled() ||
            // The asset set keys the bundle; a row-only change is re-uploaded in place by publish_hlod_bundle().
            state.hlod_instance_rows == 0 ||
            state.hlod_instance_asset_generation != director->get_instance_asset_generation_for_renderer(this) ||
            state.hlod_grading_generation != state.instance_grading_defaults_generation.load(std::memory_order_relaxed) ||
            subsystem_state.sorting_pipeline.is_null() || subsystem_state.sorting_pipeline->get_sort_resource_device() != rd) {
        return false;
    }
    const bool quantized = g_quantization_config.per_chunk_quantization;
    if (quantized && (!input->quantized_eligible ||
            state.hlod_position_bits != MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX) ||
            state.hlod_scale_bits != MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX) ||
            state.hlod_quantize_scales != g_quantization_config.quantize_scales)) { return false; }
    // PR1b: capacities include the packed node assets (an empty region gives PR1a's min(leaf_count, max_splats) and
    // leaf_count exactly); a max_splats change that moves them rebuilds.
    const gs_hlod_bundle::InstanceRegion &region = state.hlod_instance_region;
    const uint32_t relative_capacity = gs_hlod_bundle::relative_capacity(input->leaf_count, get_performance_settings().max_splats, get_hlod_view_splat_cap(), region);
    const uint32_t canonical_capacity = gs_hlod_bundle::canonical_capacity(input->leaf_count, region);
    if (relative_capacity != state.hlod_relative_capacity || canonical_capacity != state.hlod_canonical_capacity) { return false; }
    const SortBufferHandles sort = subsystem_state.sorting_pipeline->get_buffer_handles();
    if (!sort.valid || sort.capacity < MAX(canonical_capacity, relative_capacity) ||
            !rd->buffer_is_valid(sort.keys_buffer) || !rd->buffer_is_valid(sort.indices_buffer)) { return false; }
    const bool streaming = state.hlod_streaming;
    if (streaming && hlod_streaming_unsupported_reason(nullptr) != nullptr) { return false; } // Release; republish decides again.
    if (streaming && (state.hlod_streamer.is_null() || !state.hlod_streamer->is_active() ||
            state.hlod_streamer->get_residency().device_generation() != rd->get_device_instance_id() ||
            state.hlod_streamer->get_residency().world_generation() != uint64_t(input->get_instance_id()) ||
            state.hlod_stream_atlas_records == 0)) { return false; }
    for (bool relative : { false, true }) {
        if (streaming && !relative) { continue; } // A streamed world has no canonical full-leaf copy.
        const auto &bundle = relative ? state.hlod_relative : state.hlod_canonical;
        const auto &b = bundle.buffers;
        if (bundle.generation != uint64_t(input->get_instance_id()) || bundle.device_id != rd->get_device_instance_id() ||
                bundle.quantized != quantized || b.quantization_required != quantized || b.hlod_relative != relative ||
                b.hlod_source_generation != bundle.generation || b.instance_count != state.hlod_instance_rows ||
                uint64_t(b.atlas_gaussian_count) != (streaming ? uint64_t(state.hlod_stream_atlas_records) : uint64_t(input->leaf_count) + (relative ? input->parent_count : 0)) + region.records ||
                b.max_visible_splats != (relative ? relative_capacity : canonical_capacity) ||
                uint64_t(b.max_visible_chunks) != gs_hlod_bundle::visible_chunk_capacity(input->nodes.size(), relative, region) ||
                uint64_t(bundle.draw_entries.size()) != (relative ? gs_hlod_bundle::visible_chunk_capacity(input->nodes.size(), true, region) : 0u) ||
                bundle.owned_count != uint32_t(12 + int(relative) + int(quantized)) ||
                bundle.bytes != hlod_bundle_bytes(*input.ptr(), relative, quantized, b.max_visible_splats, state.hlod_instance_rows,
                        region, streaming ? state.hlod_stream_atlas_records : 0)) { return false; }
        for (uint32_t i = 0; i < bundle.owned_count; ++i) {
            if (!bundle.owned[i].is_valid() || get_resource_owner(bundle.owned[i], nullptr) != rd ||
                    !rd->buffer_is_valid(bundle.owned[i])) { return false; }
        }
        auto ready = b;
        ready.sort_key_buffer = sort.keys_buffer;
        ready.sort_value_buffer = sort.indices_buffer;
        // A prepared relative bundle has no cut content serial until its first
        // publication. Check resources here; the full published-content contract
        // is checked after assigning the real serial in publish_hlod_bundle().
        if (relative && (!b.hlod_draw_entry_buffer.is_valid() ||
                get_resource_owner(b.hlod_draw_entry_buffer, nullptr) != rd ||
                !rd->buffer_is_valid(b.hlod_draw_entry_buffer))) { return false; }
        if (!GaussianSplatting::InstancePipelineContract::has_atlas_buffers(ready) ||
                !GaussianSplatting::InstancePipelineContract::has_cull_buffers(ready) ||
                GaussianSplatting::InstancePipelineContract::first_sort_violation(ready) != InvariantViolationReason::NONE ||
                GaussianSplatting::InstancePipelineContract::first_raster_violation(ready) != InvariantViolationReason::NONE) { return false; }
    }
    const uint64_t budget = rd->get_device_memory_budget();
    const uint64_t limit = DeviceVramLedger::gs_limit_for_device(budget, GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT);
    const uint32_t override_mb = gs::settings::get_uint(ProjectSettings::get_singleton(), "rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb", 0u);
    const uint64_t atlas_limit = DeviceVramLedger::atlas_limit_for_device(budget, override_mb);
    uint64_t bytes = 0;
    return ((streaming ? uint64_t(state.hlod_stream_atlas_records) : uint64_t(input->leaf_count + input->parent_count)) + region.records) *
                    (quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian)) <= atlas_limit &&
            get_hlod_owned_byte_upper_bound(bytes) &&
            ResidentAtlasBudget::hlod_prepared_fits(state.hlod_canonical.bytes + state.hlod_relative.bytes, bytes, limit, GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT);
}

bool GaussianSplatRenderer::idle_hlod_streaming_bundle(bool p_with_instances, uint64_t p_origin_revision) {
    auto &state = get_resource_state();
    auto &bundle = state.hlod_relative;
    RenderingDevice *rd = get_device_state().rd;
    // A failure keeps a specific cause that is already there, and otherwise names itself (it used to leave last frame's reason).
    auto idle_failed = [&](const char *p_reason) {
        if (state.hlod_reason.is_empty() || state.hlod_reason == "active" || state.hlod_reason == "prepared_awaiting_view_admission" ||
                state.hlod_reason == "hlod_publish_failed") {
            state.hlod_reason = p_reason;
        }
        return false;
    };
    if (!state.hlod_streaming || !rd || !bundle.buffers.counter_buffer.is_valid() ||
            get_resource_owner(bundle.buffers.counter_buffer, nullptr) != rd) {
        return idle_failed("hlod_idle_bundle_unavailable");
    }
    // The world draws nothing: no world record. ADR 4.4: a loading world never hides the objects, so the view's instance
    // records (formed by prepare_hlod_frame for this view, offsets for `p_origin_revision`) are still drawn when given;
    // otherwise both the CPU draw count the cull stage reads and the GPU counter the sort reads are zero.
    const uint32_t instance_records = p_with_instances ? state.hlod_instance_draws.size() : 0u;
    if (instance_records > uint32_t(bundle.draw_entries.size())) { return idle_failed("hlod_idle_bundle_upload_failed"); }
    const uint32_t counters[3] = { instance_records, 0u, 0u };
    if ((instance_records && rd->buffer_update(bundle.buffers.visible_chunk_buffer, 0, instance_records * sizeof(DrawnNodeGPU),
                 state.hlod_instance_draws.ptr()) != OK) ||
            rd->buffer_update(bundle.buffers.counter_buffer, 0, sizeof(counters), counters) != OK) {
        return idle_failed("hlod_idle_bundle_upload_failed");
    }
    if (instance_records) {
        ++state.hlod_draw_list_uploads; // The draw list now holds the objects alone (a shadow caster record may name it).
    }
    bundle.buffers.hlod_draw_count = instance_records;
    if (instance_records) {
        bundle.buffers.hlod_origin_revision = p_origin_revision;
        state.hlod_origin_revision = p_origin_revision;
    }
    const bool published = publish_hlod_bundle(true);
    state.hlod_active = false; // The restore after this frame idles again instead of re-binding a stale list.
    return published;
}

bool GaussianSplatRenderer::publish_hlod_bundle(bool p_relative) {
    auto &state = get_resource_state();
    if (state.hlod_streaming && !p_relative) {
        return idle_hlod_streaming_bundle();
    }
    auto &bundle = p_relative ? state.hlod_relative : state.hlod_canonical;
    // This is also the only restore entry. Never bind retained A after current B
    // has fallen back, even if the caller captured its restore intent before B.
    if (!is_hlod_prepared_contract_current()) {
        if (state.hlod_prepared_snapshot.is_valid()) {
            release_hlod_resources();
            auto *lost_director = GaussianSplatSceneDirector::get_singleton();
            state.hlod_reason = (lost_director && !lost_director->has_hlod_world_submission(this, state.hlod_owner_key))
                    ? "hlod_world_submission_lost" : "hlod_prepared_identity_or_readiness_changed";
        }
        return false;
    }
    // Row content moved without the asset set (a transform, parameter or grading edit): re-upload the rows in place.
    // A single-row world never gets here with a moved row generation (every bump it can see also moves the asset or the
    // grading-default generation, which the contract check above refuses).
    if (!refresh_hlod_instance_rows()) {
        release_hlod_resources();
        state.hlod_reason = "hlod_instance_rows_refresh_failed";
        return false;
    }
    if (!instance_asset_remap.valid || instance_asset_remap.generation != bundle.generation ||
            state.hlod_remap_asset_generation != state.hlod_instance_asset_generation) {
        // Every asset the bundle packs, at its dense id: the world at 0, the node assets of the instance region at 1..M.
        instance_asset_remap.asset_to_dense_id.clear();
        instance_asset_remap.asset_to_dense_id.insert(gs_hlod_bundle::WORLD_ASSET_ID, gs_hlod_bundle::WORLD_DENSE_ID);
        for (uint32_t a = 0; a < state.hlod_instance_region.assets.size(); ++a) {
            instance_asset_remap.asset_to_dense_id.insert(state.hlod_instance_region.assets[a].asset_id, a + 1u);
        }
        instance_asset_remap.generation = bundle.generation;
        instance_asset_remap.valid = true;
        state.hlod_remap_asset_generation = state.hlod_instance_asset_generation;
    }
    const SortBufferHandles handles = subsystem_state.sorting_pipeline->get_buffer_handles();
    bundle.buffers.sort_key_buffer = handles.keys_buffer;
    bundle.buffers.sort_value_buffer = handles.indices_buffer;
    bundle.buffers.hlod_content_generation = ++state.hlod_content_serial;
    if (!GaussianSplatting::InstancePipelineContract::has_sort_buffers(bundle.buffers) ||
            !GaussianSplatting::InstancePipelineContract::has_raster_buffers(bundle.buffers)) {
        release_hlod_resources();
        state.hlod_reason = "hlod_published_content_contract_incomplete";
        return false;
    }
    instance_pipeline_buffers = bundle.buffers;
    instance_backend_policy = InstanceBackendPolicy::RESIDENT;
    instance_contract_source_generation = bundle.buffers.hlod_content_generation;
    instance_contract_shape = p_relative ? "hlod_relative" : "hlod_canonical";
    instance_pipeline_buffers_valid = true;
    state.hlod_active = p_relative;
    state.instance_pipeline_content_generation = bundle.buffers.hlod_content_generation;
    return true;
}

bool GaussianSplatRenderer::refresh_hlod_instance_rows() {
    auto &state = get_resource_state();
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    RenderingDevice *rd = get_device_state().rd;
    if (!director || !rd) { return false; }
    const uint64_t row_generation = director->get_instance_generation_for_renderer(this);
    if (row_generation == state.hlod_instance_generation) { return true; }
    gs_hlod_bundle::Rows rows;
    LocalVector<uint64_t> keys;
    director->build_hlod_instance_rows_for_renderer(this, rows.instances, rows.grading, rows.asset_ids, keys);
    const uint64_t asset_generation = director->get_instance_asset_generation_for_renderer(this);
    // A moving node lands here (PR1b): same asset set, same row count, rows naming the same assets (rows_refusal checks the
    // row tallies against the packed region), so only the rows are re-uploaded; its draw offsets are formed per view.
    if (gs_hlod_bundle::prepared_rows_action(state.hlod_instance_asset_generation, asset_generation, state.hlod_instance_generation,
                row_generation, state.hlod_instance_rows, rows.count()) != gs_hlod_bundle::PreparedRows::REUPLOAD ||
            state.hlod_prepared_snapshot.is_null() ||
            gs_hlod_bundle::rows_refusal(rows, uint64_t(state.hlod_prepared_snapshot->nodes.size()), state.hlod_instance_region) != nullptr) {
        return false;
    }
    LocalVector<InstanceDataGPU> packed;
    gs_hlod_bundle::pack_instance_rows(rows, state.hlod_instance_region, packed);
    for (bool relative : { false, true }) {
        if (state.hlod_streaming && !relative) { continue; } // A streamed world has no canonical bundle.
        const auto &b = (relative ? state.hlod_relative : state.hlod_canonical).buffers;
        if (b.instance_count != rows.count() ||
                rd->buffer_update(b.instance_buffer, 0, rows.count() * sizeof(InstanceDataGPU), packed.ptr()) != OK ||
                rd->buffer_update(b.instance_grading_buffer, 0, rows.count() * sizeof(InstanceGradingGPU), rows.grading.ptr()) != OK) {
            return false;
        }
    }
    // The generation read BEFORE the rows were built: a bump in between is re-uploaded next time, never skipped.
    state.hlod_instance_generation = row_generation;
    state.hlod_instance_row_data = rows;
    state.hlod_instance_row_keys = keys;
    ++state.hlod_instance_row_reuploads;
    return true;
}

uint32_t GaussianSplatRenderer::get_hlod_view_splat_cap() const {
    const int max_splats = MAX(1, get_performance_settings().max_splats);
    const auto &state = get_resource_state();
    if (state.hlod_view_splat_cap > 0 && max_splats == state.hlod_view_splat_cap_clamped) {
        return uint32_t(MAX(max_splats, state.hlod_view_splat_cap));
    }
    return uint32_t(max_splats);
}

String GaussianSplatRenderer::get_hlod_instance_refusal(uint64_t p_instance_key) {
    auto &state = get_resource_state();
    MutexLock lock(state.hlod_instance_refusal_mutex);
    for (uint64_t key : state.hlod_instance_refused_keys) {
        if (key == p_instance_key) { return String(gs_hlod_bundle::INSTANCE_REFUSAL_REASON); }
    }
    return String();
}

bool GaussianSplatRenderer::request_hlod_auto_route_demotion(const gs_hlod_auto_route::RefusalInputs &p_refusal) {
    auto &state = get_resource_state();
    if (state.hlod_auto_route_demotion.requested) { return false; }
    const char *cause = gs_hlod_auto_route::demotion_cause(p_refusal);
    if (cause == nullptr) { return false; }
    // One request per submitted snapshot, also when nothing could be queued (no node callback): never every frame.
    state.hlod_auto_route_demotion.request(cause);
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director || !director->request_hlod_auto_route_demotion(this, state.hlod_owner_key, state.hlod_snapshot, String(cause))) {
        return false;
    }
    ++state.hlod_auto_route_demotions;
    state.hlod_reason = String(gs_hlod_auto_route::AUTO_ROUTE_DEMOTED_PREFIX) + cause;
    return true;
}

const char *GaussianSplatRenderer::hlod_streaming_unsupported_reason(const InstanceDataGPU *p_instance) const {
    // Conditions under which the HLOD cut can never become active; a streamed bundle would then draw nothing for
    // ever, so these setups keep the whole-tree / legacy resident route (reason keys as in prepare_hlod_frame).
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!get_lod_enabled()) { return "lod_disabled_canonical_leaves"; }
    if (is_shadow_instance_filter_enabled()) { return "shadow_instance_filter"; }
    if (get_painterly_config().enabled) { return "unsupported_view_or_material"; } // Persistent renderer setting, known at publish.
    if (gs::settings::get_wind_settings(ps).enabled || gs::settings::get_sphere_effector_settings(ps, true).enabled) { return "deformation_or_instance_domain"; }
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director || director->get_sphere_effector_count_for_renderer(this) != 0) { return "deformation_or_instance_domain"; }
    if (p_instance) {
        const bool identity = (p_instance->ids[1] & GS_INSTANCE_FLAG_ROTATION_IDENTITY) != 0 && p_instance->translation_scale[0] == 0.0f &&
                p_instance->translation_scale[1] == 0.0f && p_instance->translation_scale[2] == 0.0f && p_instance->translation_scale[3] == 1.0f;
        if (!identity) { return "unsupported_instance_transform_domain"; }
    }
    return nullptr;
}

// Reason keys of a V4 paged world. A paged world has no canonical copy to fall back to, so the resident keys
// (lod_disabled_canonical_leaves, ...), which promise "the canonical leaves are drawn", would mislead. At publish time
// the painterly refusal shares the resident key unsupported_view_or_material; per frame (p_publish false) that key keeps
// its D1 meaning (orthographic / XR / non-main pass) and is not renamed: those frames draw nothing for that view only.
static const char *hlod_paged_reason_key(const char *p_base, bool p_publish) {
    const String base = p_base;
    if (base == "lod_disabled_canonical_leaves") { return "hlod_paged_lod_disabled"; }
    if (base == "shadow_instance_filter") { return "hlod_paged_shadow_instance_filter"; }
    if (base == "deformation_or_instance_domain") { return "hlod_paged_deformation_or_instance_domain"; }
    if (base == "unsupported_instance_transform_domain") { return "hlod_paged_non_identity_instance_transform"; }
    if (p_publish && base == "unsupported_view_or_material") { return "hlod_paged_painterly_enabled"; }
    return p_base;
}

// The route's device-ledger limit exactly as prepare_hlod_frame() derives it (including the test-only override).
// What `p_views` ViewSet views of the HLOD route charge against the route's headroom (snapshot budget physical_bytes):
// each holds one world instance's history (ViewSet::slot_bytes) and its draw records for node_budget = the world's nodes
// (ViewSet::view_cost; the route's ResidencyBudget prices nothing per splat, record or node). UINT64_MAX when unprovable.
static uint64_t hlod_view_reservation_bytes(uint32_t p_world_nodes, uint32_t p_views) {
    using gs_hlod_runtime::ViewSet;
    gs_hlod_runtime::Caps caps;
    caps.splat_budget = 1;
    caps.node_budget = MAX<uint32_t>(p_world_nodes, 1u);
    const uint64_t history = ViewSet::slot_bytes(caps.node_budget);
    uint64_t one = 0;
    if (history == 0 || !ViewSet::view_cost(gs_hlod_runtime::ResidencyBudget(), caps, history, caps.node_budget, one)) { return UINT64_MAX; }
    return one != 0 && p_views > UINT64_MAX / one ? UINT64_MAX : one * p_views;
}

// The HLOD frame route's limit. Only the frame route honours the test override; a publication does not.
static uint64_t hlod_route_limit_bytes(GaussianRenderFacadeState::ResourceState &p_state, RenderingDevice *p_rd) {
    uint64_t limit_clamp = 0;
#ifdef TESTS_ENABLED
    limit_clamp = p_state.hlod_test_route_limit_override.load(std::memory_order_acquire);
#endif
    return refresh_device_vram_ledger(p_state, p_rd, limit_clamp).limit();
}

// The category split of the measured device bytes, once the frame is admitted. It only explains the measurement: what it
// misses stays unattributed, and no admission reads it.
static void report_hlod_ledger_categories(GaussianRenderFacadeState::ResourceState &p_state, const GaussianSplatHlodRuntimeData &p_input,
        const TileRenderer &p_tile, uint64_t p_textures, const SortBufferHandles &p_sort, uint64_t p_shadow_caster_bytes) {
    DeviceVramLedger &ledger = p_state.device_vram_ledger;
    const uint64_t stride = g_quantization_config.per_chunk_quantization ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
    const bool has_canonical = p_state.hlod_canonical.bytes != 0;
    const uint64_t relative_world_records = p_state.hlod_streaming ? uint64_t(p_state.hlod_stream_atlas_records) :
            uint64_t(p_input.leaf_count) + p_input.parent_count;
    const uint64_t pool_bytes = (relative_world_records + (has_canonical ? uint64_t(p_input.leaf_count) : 0u)) * stride;
    const uint64_t region_bytes = uint64_t(p_state.hlod_instance_region.records) * stride * (has_canonical ? 2u : 1u);
    const uint64_t bundle_bytes = p_state.hlod_canonical.bytes + p_state.hlod_relative.bytes;
    const uint64_t atlas_bytes = pool_bytes + region_bytes;
    const uint64_t overlap_per_record = p_tile.get_hlod_overlap_bytes_per_record();
    ledger.report(DeviceVramLedger::POOL, pool_bytes);
    ledger.report(DeviceVramLedger::INSTANCE_REGION, region_bytes);
    ledger.report(DeviceVramLedger::VIEW, bundle_bytes > atlas_bytes ? bundle_bytes - atlas_bytes : 0u);
    ledger.report(DeviceVramLedger::OVERLAP, uint64_t(p_tile.get_allocated_overlap_capacity()) * overlap_per_record);
    // One sort record costs one overlap record without the sorter's alternate buffer (same key width).
    ledger.report(DeviceVramLedger::SORT, uint64_t(p_sort.capacity) * (overlap_per_record / 2u));
    ledger.report(DeviceVramLedger::HLOD_SCRATCH, p_tile.get_hlod_scratch_live_bytes());
    ledger.report(DeviceVramLedger::TARGET_TEXTURES, p_textures);
    ledger.report(DeviceVramLedger::OBJECT_ATLAS, p_state.resident_atlas_gaussian_buffer_size);
    ledger.report(DeviceVramLedger::SHADOW_CASTER, p_shadow_caster_bytes);
}

void GaussianSplatRenderer::report_chunk_route_to_device_vram_ledger(uint64_t p_chunk_bytes) {
    RenderingDevice *rd = get_device_state().rd;
    if (!rd) { return; }
    // Report-only until Step C (D-11): nothing on the chunk route reads this ledger. MEMORY_BUFFERS is a counter read; the
    // HLOD upper bound would also reconcile the tile texture ledger, which the chunk route does not need.
    DeviceVramLedger &ledger = refresh_device_vram_ledger(get_resource_state(), rd);
    ledger.observe_device_bytes(rd->get_memory_usage(RD::MEMORY_BUFFERS));
    ledger.report(DeviceVramLedger::CHUNK_POOL, p_chunk_bytes);
}

bool GaussianSplatRenderer::provision_paged_hlod_route(RenderSceneBuffersRD *p_render_buffers, String &r_reason) {
    auto &state = get_resource_state();
    const Ref<GaussianSplatHlodRuntimeData> input = state.hlod_snapshot;
    if (input.is_null() || !input->is_paged()) { return true; }
    RenderingDevice *rd = get_device_state().rd;
    Ref<TileRenderer> tile = tile_renderer_state.renderer;
    OutputCompositor *compositor = subsystem_state.output_compositor.ptr();
    if (!rd || tile.is_null() || !compositor || !p_render_buffers) {
        r_reason = "hlod_paged_provisioning_needs_render_target";
        state.hlod_paged_provision_reason = r_reason;
        return false;
    }
    const Size2i size = p_render_buffers->get_internal_size();
    const RID color = p_render_buffers->get_internal_texture();
    if (size.x <= 0 || size.y <= 0 || !color.is_valid()) {
        r_reason = "hlod_paged_provisioning_needs_render_target";
        state.hlod_paged_provision_reason = r_reason;
        return false;
    }
    // The compositor scratch is a pool entry that eviction, a resize or a device change can drop: keep it present (a pool
    // hit when it is) and the ledger live.
    if (!compositor->ensure_hlod_scratch(rd, rd->texture_get_format(color), size)) {
        r_reason = "hlod_paged_compositor_scratch_failed";
        state.hlod_paged_provision_reason = r_reason;
        return false;
    }
    // The tile grid is not provisioned here: prepare_hlod_frame's per-view provisioner (hlod_view_grid_action) sizes it for
    // every view of a streamed or paged world, per view and per frame. Only the first overlap seed stays: the publication
    // needs the tile ledger and overlap storage before the route's first frame. The relative route never grows the overlap
    // storage while drawing, so seed it as a canonical warmup frame of the same world would (every leaf may be visible, 50
    // records each; the sorting config caps it); growth afterwards is the measured safe-point growth in prepare_hlod_frame.
    if (tile->get_allocated_overlap_capacity() == 0) {
        // Priced exactly like prepare_hlod_frame's per-view seed (same clamp, same bytes per record, same decision): the
        // grid for this view plus the seeded records must fit the route's ledger headroom, or nothing is allocated and the
        // route says why. Unpriced, a tree of millions of leaves seeded GBs past the limit and was refused for good after.
        const uint32_t overlap = tile->clamp_hlod_overlap_records(uint32_t(MIN<uint64_t>(uint64_t(input->leaf_count) * 50u, UINT32_MAX)));
        uint64_t route_bytes = 0, live_textures = 0;
        if (!get_hlod_owned_byte_upper_bound(route_bytes) || !tile->get_hlod_texture_byte_upper_bound(live_textures)) {
            r_reason = "hlod_paged_resource_ledger_unavailable";
            state.hlod_paged_provision_reason = r_reason;
            return false;
        }
        hlod_route_limit_bytes(state, rd);
        state.device_vram_ledger.observe_device_bytes(route_bytes);
        const Vector2i grid = tile->get_provisioned_viewport();
        const uint64_t wanted_bytes = tile->estimate_hlod_texture_bytes_for_viewport(size) +
                uint64_t(overlap) * tile->get_hlod_overlap_bytes_per_record();
        if (ResidentAtlasBudget::hlod_view_grid_action(grid.x, grid.y, size.x, size.y, 0u, live_textures, wanted_bytes,
                    state.device_vram_ledger.headroom()) == ResidentAtlasBudget::HlodViewGridAction::REFUSE_MEMORY) {
            r_reason = "hlod_paged_overlap_seed_memory_limit";
            state.hlod_paged_provision_reason = r_reason;
            return false;
        }
        if (tile->provision_hlod_route(size, RD::DATA_FORMAT_MAX, overlap) != OK) {
            r_reason = "hlod_paged_tile_provisioning_failed";
            state.hlod_paged_provision_reason = r_reason;
            return false;
        }
    }
    state.hlod_paged_provision_reason = String();
    return true;
}

int GaussianSplatRenderer::try_publish_hlod_resident_contract(String &r_reason) {
    auto &state = get_resource_state();
    // A world with committed edits states the CURRENT cause on every blank frame: start from a generic one, so a return that
    // sets no reason of its own never keeps last frame's cause (or last frame's "active").
    if (hlod_edit_committed_here()) { state.hlod_reason = "hlod_publish_failed"; }
    auto *director = GaussianSplatSceneDirector::get_singleton();
    const Ref<GaussianSplatHlodRuntimeData> input = state.hlod_snapshot;
    if (state.hlod_prepared_snapshot.is_valid() && !is_hlod_prepared_contract_current()) {
        release_hlod_resources();
    }
    if (!gs::settings::get_hlod_runtime_cut_enabled(ProjectSettings::get_singleton()) || input.is_null() ||
            !director || !director->has_hlod_world_submission(this, state.hlod_owner_key)) {
        if (state.hlod_prepared_snapshot.is_valid()) { release_hlod_resources(); }
        // Say why the route is not taken (a paged world has no other route; the others keep drawing canonically). Node
        // instances never refuse the route (PR1b): only a missing or foreign world submission does.
        if (input.is_valid()) {
            const bool paged = input->is_paged();
            state.hlod_reason = !gs::settings::get_hlod_runtime_cut_enabled(ProjectSettings::get_singleton())
                    ? (paged ? "hlod_paged_requires_runtime_cut" : "hlod_runtime_cut_disabled")
                    : (paged ? "hlod_paged_world_submission_inactive" : "hlod_world_submission_inactive");
        }
        return 0;
    }
    if (!input->is_current() || input->source_data != get_scene_state().gaussian_data) {
        state.hlod_reason = "hlod_snapshot_no_longer_current";
        release_hlod_resources();
        return 0;
    }
    RenderingDevice *rd = get_device_state().rd;
    const bool quantized = g_quantization_config.per_chunk_quantization;
    if (!rd || subsystem_state.sorting_pipeline.is_null() || subsystem_state.sorting_pipeline->get_sort_resource_device() != rd) {
        state.hlod_reason = "hlod_current_device_or_sort_resources_unavailable";
        release_hlod_resources();
        return 0;
    }
    if (quantized && !input->quantized_eligible) {
        state.hlod_reason = "hlod_quantized_mixed_dc_node";
        // A quantization switched on after the submission (the world node demotes at submission otherwise).
        gs_hlod_auto_route::RefusalInputs refusal;
        refusal.stage = gs_hlod_auto_route::RefusalStage::PUBLISH;
        refusal.reason = "hlod_quantized_mixed_dc_node";
        refusal.committed_edit = state.hlod_committed_edit.is_valid();
        request_hlod_auto_route_demotion(refusal);
        release_hlod_resources();
        return 0;
    }
    const uint64_t instance_generation = director->get_instance_generation_for_renderer(this);
    const uint64_t instance_asset_generation = director->get_instance_asset_generation_for_renderer(this);
    // PR1b: the bundle's rows (row 0 is the world, rows 1..N the visible node instances) and their packed node assets.
    // A current prepared bundle keeps its own (a moving node only re-uploads rows); anything else builds them here,
    // before the capacities, which depend on the assets the rows name.
    gs_hlod_bundle::Rows rows;
    LocalVector<uint64_t> row_keys;
    HlodInstancePayload instance_payload;
    const bool reuse_prepared_rows = state.hlod_prepared_snapshot.is_valid();
    if (!reuse_prepared_rows) {
        director->build_hlod_instance_rows_for_renderer(this, rows.instances, rows.grading, rows.asset_ids, row_keys);
        if (rows.count() == 0 || rows.asset_ids[gs_hlod_bundle::WORLD_ROW] != gs_hlod_bundle::WORLD_ASSET_ID) {
            state.hlod_reason = "hlod_world_row_unavailable";
            return 0;
        }
        LocalVector<InstanceAssetRegistration> registered;
        if (rows.count() > 1) { director->collect_registered_assets_for_renderer(this, registered); }
        String pack_reason;
        if (!hlod_pack_instance_assets(registered, rows, uint32_t(input->nodes.size()), quantized, instance_payload, pack_reason)) {
            state.hlod_reason = pack_reason;
            return 0;
        }
        if (const char *rows_reason = gs_hlod_bundle::rows_refusal(rows, uint64_t(input->nodes.size()), instance_payload.region)) {
            state.hlod_reason = rows_reason;
            return 0;
        }
    }
    const gs_hlod_bundle::InstanceRegion &region = reuse_prepared_rows ? state.hlod_instance_region : instance_payload.region;
    const uint32_t relative_capacity = gs_hlod_bundle::relative_capacity(input->leaf_count, get_performance_settings().max_splats, get_hlod_view_splat_cap(), region);
    const uint32_t canonical_capacity = gs_hlod_bundle::canonical_capacity(input->leaf_count, region);
    if (canonical_capacity == UINT32_MAX) {
        state.hlod_reason = "hlod_instance_region_exceeds_capacity";
        return 0;
    }
    const uint64_t grading_generation = state.instance_grading_defaults_generation.load(std::memory_order_relaxed);
    const uint64_t owner_key = state.hlod_owner_key;
    // Package F1: a world with committed edits is rebuilt from its last committed snapshot, never from the unedited one.
    if (state.hlod_committed_edit.is_valid() && !hlod_committed_edit_applies_to(input)) {
        release_hlod_committed_edit();
    }
    Ref<GaussianSplatHlodRuntimeData> served = input;
    if (state.hlod_committed_edit.is_valid()) {
        const Ref<GaussianSplatHlodRuntimeData> edit = state.hlod_committed_edit;
        if (edit->source_world == input->source_world && edit->source_revision == input->source_revision &&
                edit->nodes.size() == input->nodes.size() && edit->is_paged() == input->is_paged()) {
            served = edit;
        } else {
            // A fresh capture of the same world (the cut was toggled and a V4 world was materialized into the resident
            // shape, or the world was re-submitted): lay the committed edits over it again, never serve it unedited.
            Ref<GaussianSplatHlodRuntimeData> rebased;
            if (gs_hlod_runtime::rebase_edited_snapshot(edit, input, rebased)) {
                state.hlod_committed_edit = rebased;
                served = rebased;
            }
        }
    }
    const uint32_t position_bits = MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX);
    const uint32_t scale_bits = MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX);
    const bool quantize_scales = g_quantization_config.quantize_scales;
    // One sort holds the world and the objects (PR1b): the canonical bundle sorts every leaf and every row's asset.
    const uint32_t sort_capacity = MAX(canonical_capacity, relative_capacity);
    SortBufferHandles existing_sort = subsystem_state.sorting_pipeline.is_valid() ? subsystem_state.sorting_pipeline->get_buffer_handles() : SortBufferHandles();
    // An edited world suppresses the canonical publication too (it would publish the unedited copy), so it also sizes the sorter
    // here instead of waiting for a canonical frame that never comes. So does a world with node rows: the canonical
    // resident publication (with node instances it packs no world row) never sizes the sorter for the world's leaves.
    if ((input->is_paged() || hlod_edit_committed_here() || region.row_splats != 0) && subsystem_state.sorting_pipeline.is_valid() &&
            (!existing_sort.valid || existing_sort.capacity < sort_capacity) && !state.hlod_prepared_snapshot.is_valid()) {
        // A paged world has no canonical atlas, so the canonical publication that sizes the sort buffers for every
        // other world never runs. Its need is known here (every leaf may be sorted): size the sorter from it. No
        // contract is published on the old handles, so nothing goes stale. The sort scratch still scales with the whole
        // tree: GPU memory, not CPU payload memory, and not changed by E1.
        if (sort_capacity > subsystem_state.sorting_pipeline->get_max_elements()) {
            subsystem_state.sorting_pipeline->rebuild_sorter(sort_capacity);
        }
        subsystem_state.sorting_pipeline->ensure_buffers(sort_capacity);
        existing_sort = subsystem_state.sorting_pipeline->get_buffer_handles();
    }
    if (
#ifdef TESTS_ENABLED
            test_hlod_publication_refuses(1) ||
#endif
            !existing_sort.valid || existing_sort.capacity < sort_capacity) {
        state.hlod_reason = input->is_paged()
                ? (!state.hlod_paged_provision_reason.is_empty() ? state.hlod_paged_provision_reason : String("hlod_paged_sort_allocation_failed"))
                : (hlod_edit_committed_here() ? String("hlod_edit_sort_allocation_failed") : String("canonical_sort_capacity_warmup"));
        return 0;
    }
    const bool prepared = is_hlod_prepared_contract_current();
    if (!prepared) {
        DeviceVramLedger &ledger = refresh_device_vram_ledger(state, rd);
        const uint64_t route_limit = ledger.limit();
        const uint32_t atlas_override = gs::settings::get_uint(ProjectSettings::get_singleton(), "rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb", 0u);
        const uint64_t atlas_limit = ledger.atlas_limit(atlas_override);
        const uint64_t record_stride = quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
        // The bundle's rows (row 0 the world, rows 1..N the node instances) and their packed assets were built above. A
        // prepared bundle that stopped being current in between cannot lend its rows to a rebuild: release, rebuild next frame.
        if (reuse_prepared_rows) {
            release_hlod_resources();
            state.hlod_reason = "hlod_prepared_identity_or_readiness_changed";
            return 0;
        }
        const uint32_t row_count = rows.count();
        uint64_t device_bytes = 0;
        if (
#ifdef TESTS_ENABLED
                test_hlod_publication_refuses(2) ||
#endif
                !get_hlod_owned_byte_upper_bound(device_bytes)) {
            state.hlod_reason = (input->is_paged() && !state.hlod_paged_provision_reason.is_empty())
                    ? state.hlod_paged_provision_reason : String("resource_ledger_warmup_or_unavailable");
            return 0;
        }
        // One ledger (ADR 4.5): the node assets (charged once per distinct asset), their rows and draw slots are inside
        // the candidate bytes, so the admission below charges them before anything is allocated.
        const uint64_t whole_candidate_bytes = hlod_bundle_bytes(*input.ptr(), false, quantized, canonical_capacity, row_count, region) +
                hlod_bundle_bytes(*input.ptr(), true, quantized, relative_capacity, row_count, region);
        const uint64_t instance_atlas_bytes = region.records * record_stride;
        const uint64_t whole_atlas_bytes = uint64_t(input->leaf_count + input->parent_count) * record_stride + instance_atlas_bytes;
        // The tree streams whenever its whole-tree bundles cannot be admitted (atlas budget, HLOD-owned cap or
        // the device ledger), never because a limit was raised; every limit stays as it is. A setup that can
        // never activate the cut (LOD off, deformation, non-identity placement, shadow filter) keeps the
        // whole-tree/legacy behaviour instead: a streamed world has no canonical copy to fall back to.
        const bool whole_fits = whole_atlas_bytes <= atlas_limit &&
                ResidentAtlasBudget::hlod_candidate_fits(whole_candidate_bytes, device_bytes, route_limit, GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT);
        // A paged snapshot holds no payload at all: it can only ever stream, and where streaming cannot activate
        // there is no whole-tree or canonical copy to fall back to, so it is refused with the reason.
        const char *streaming_unsupported = hlod_streaming_unsupported_reason(&rows.instances[gs_hlod_bundle::WORLD_ROW]);
        const bool should_stream = ResidentAtlasBudget::hlod_should_stream(whole_atlas_bytes, atlas_limit, whole_fits);
        if (input->is_paged() && streaming_unsupported != nullptr) {
            // The world node materializes the payload and uses the resident route for exactly these cases when it can
            // see them at submission (GaussianSplatWorld::materialize_paged_resident); this is the late/dynamic case, and
            // the automatic route demotes the world to that same load (lod/hlod_auto_route.h).
            state.hlod_reason = hlod_paged_reason_key(streaming_unsupported, true);
            gs_hlod_auto_route::RefusalInputs refusal;
            refusal.stage = gs_hlod_auto_route::RefusalStage::PUBLISH;
            refusal.reason = streaming_unsupported;
            refusal.no_canonical_copy = true;
            refusal.committed_edit = state.hlod_committed_edit.is_valid();
            if (!request_hlod_auto_route_demotion(refusal)) {
                WARN_PRINT_ONCE(vformat("[GaussianSplatRenderer] A V4 paged world cannot use the HLOD route (%s) and draws nothing; "
                                        "load a resident (v3) copy or fix the setting.", state.hlod_reason));
            }
            return 0;
        }
        if (streaming_unsupported != nullptr && should_stream) {
            // A world too large for its whole tree, in a setup that cannot stream: the whole-tree build below is refused by
            // its own limits and the canonical copy is the thinned resident one, where the route it had without the
            // automatic route streams it whole. Demote it (an edited world never is: decide_hlod_route refuses it).
            gs_hlod_auto_route::RefusalInputs refusal;
            refusal.stage = gs_hlod_auto_route::RefusalStage::PUBLISH;
            refusal.reason = streaming_unsupported;
            refusal.no_canonical_copy = true;
            refusal.committed_edit = state.hlod_committed_edit.is_valid();
            if (request_hlod_auto_route_demotion(refusal)) {
                return 0;
            }
        }
        // Package F1 (F-1): an edited world is served by the streamer ONLY (a whole-tree bundle packs the unedited payload and the
        // canonical copy is the unedited world). So a committed edit forces streaming, and where streaming is unsupported, or the
        // committed edit could not be laid over this capture (rebase refused), no route is built at all: the reason says so and
        // the next frame tries again, instead of building a route over the unedited data that would then blank for ever.
        String route_reason;
        const HlodRouteDecision route = decide_hlod_route(state.hlod_committed_edit.is_valid(), served->has_edits(), input->is_paged(),
                should_stream, streaming_unsupported, route_reason);
        if (route == HlodRouteDecision::REFUSE) {
            state.hlod_reason = route_reason;
            return 0;
        }
        const bool streaming = route == HlodRouteDecision::BUILD_STREAMED;
        uint32_t stream_records = 0;
        HlodNodeStreamer::Config stream_config;
        uint64_t candidate_bytes = whole_candidate_bytes;
        if (streaming) {
            uint64_t node_pages = 0;
            for (int n = 0; n < served->nodes.size(); ++n) {
                node_pages += (uint64_t(served->nodes[n].topology.payload_count) + stream_config.page_splats - 1u) / stream_config.page_splats;
            }
            // Bytes of the relative bundle that do not scale with the atlas (everything but the atlas records).
            // The instance region (PR1b) is part of these fixed bytes: it sits at the atlas tail, so the page budget shrinks
            // by it and the ledger is never exceeded.
            const uint64_t fixed_bytes = hlod_bundle_bytes(*input.ptr(), true, quantized, relative_capacity, row_count, region, 1) - record_stride;
            // The frame scratch is admitted later from the same ledger headroom: reserve it here, or a ledger-bound
            // page budget leaves the scratch admission refusing every frame.
            uint64_t scratch_fixed = 0, scratch_per_record = 0;
            TileRenderer::get_hlod_scratch_reservation(relative_capacity, scratch_fixed, scratch_per_record);
            // PR1b: the instance region's records get their frame scratch too (prepare_hlod_scratch sizes it from world + region).
            const uint64_t pages = ResidentAtlasBudget::hlod_stream_page_budget_with_instances(atlas_limit, region.records,
                    record_stride, stream_config.page_splats,
                    node_pages, fixed_bytes, GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT, route_limit, device_bytes,
                    scratch_fixed, scratch_per_record);
            if (pages == 0 || pages * stream_config.page_splats > UINT32_MAX) {
                state.hlod_reason = whole_atlas_bytes > atlas_limit ? "hlod_existing_atlas_limit" : "hlod_candidate_memory_limit";
                return 0;
            }
            stream_config.atlas_pages = uint32_t(pages);
            stream_config.quantized = quantized;
            stream_config.position_bits = position_bits;
            stream_config.scale_bits = scale_bits;
            stream_records = uint32_t(pages * stream_config.page_splats);
            candidate_bytes = hlod_bundle_bytes(*input.ptr(), true, quantized, relative_capacity, row_count, region, stream_records);
        } else if (whole_atlas_bytes > atlas_limit) {
            state.hlod_reason = "hlod_existing_atlas_limit"; return 0;
        }
        // Same decision as hlod_candidate_fits(): the HLOD-owned cap on the candidate, then the ledger. The reservation holds
        // until this block ends, on every path; the measurement after the build carries whatever was allocated.
        ledger.observe_device_bytes(device_bytes);
        DeviceVramLedger::ScopedReservation candidate_reservation(ledger, DeviceVramLedger::POOL);
        if (
#ifdef TESTS_ENABLED
                test_hlod_publication_refuses(3) ||
#endif
                candidate_bytes > GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT || !candidate_reservation.acquire(candidate_bytes) ||
                (state.hlod_prepared_snapshot.is_valid() && state.hlod_prepared_snapshot != input &&
                        input->owned_cpu_bytes > GaussianSplatHlodRuntimeData::CPU_BYTE_LIMIT - state.hlod_prepared_snapshot->owned_cpu_bytes)) {
            state.hlod_reason = "hlod_candidate_memory_limit";
            if (publish_hlod_bundle(false)) { return 1; }
            return 0;
        }
        HlodBundle canonical, relative;
        // A streamed world builds only the relative bundle: a canonical full-leaf copy would be the whole-world
        // materialization the streaming budget exists to avoid.
        // The bundles are identified by the CAPTURED snapshot (their generation is its object id, which the prepared-contract check
        // compares), never by the edited one: an edited snapshot is a different object and the contract would never be current.
        // Edited worlds always stream (decide_hlod_route), so the whole-tree builder never sees one; refuse loudly if it ever does.
        if (!streaming && served->has_edits()) {
            state.hlod_reason = "hlod_edit_requires_streamed_route";
            return 0;
        }
        const bool built = (streaming || hlod_build_bundle(this, rd, *input.ptr(), false, quantized, canonical_capacity, position_bits, scale_bits, rows,
                                                 instance_payload, canonical)) &&
                hlod_build_bundle(this, rd, *input.ptr(), true, quantized, relative_capacity, position_bits, scale_bits, rows, instance_payload,
                        relative, streaming, stream_records);
        if (built && streaming && served != input) {
            // An edited leaf may be larger than every captured one (up to 16,384 splats).
            relative.buffers.max_chunk_splats = hlod_dispatch_extent_for(relative.buffers.max_chunk_splats, *served.ptr());
        }
        Ref<HlodNodeStreamer> streamer;
        if (built && streaming) {
            streamer.instantiate();
            // World generation = the immutable snapshot object, device generation = the device instance.
            if (!streamer->init(served, stream_config, uint64_t(input->get_instance_id()), rd->get_device_instance_id())) {
                streamer.unref();
            }
        }
        if (!built || (streaming && streamer.is_null())) {
            release_hlod_bundle(canonical);
            release_hlod_bundle(relative);
            state.hlod_reason = "hlod_candidate_allocation_failed";
            if (publish_hlod_bundle(false)) { return 1; }
            return 0;
        }
        const SortBufferHandles handles = subsystem_state.sorting_pipeline->get_buffer_handles();
        uint64_t after_bytes = UINT64_MAX;
        if (!get_hlod_owned_byte_upper_bound(after_bytes)) { after_bytes = UINT64_MAX; }
        if (
#ifdef TESTS_ENABLED
                test_hlod_publication_refuses(5) ||
#endif
                !handles.valid || handles.capacity < sort_capacity || !input->is_current() ||
                input != state.hlod_snapshot || input->source_data != get_scene_state().gaussian_data ||
                rd != get_device_state().rd || subsystem_state.sorting_pipeline->get_sort_resource_device() != rd ||
                instance_generation != director->get_instance_generation_for_renderer(this) ||
                instance_asset_generation != director->get_instance_asset_generation_for_renderer(this) ||
                grading_generation != state.instance_grading_defaults_generation.load(std::memory_order_relaxed) ||
                quantized != g_quantization_config.per_chunk_quantization || owner_key != state.hlod_owner_key ||
                (quantized && (position_bits != MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX) ||
                        scale_bits != MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX) || quantize_scales != g_quantization_config.quantize_scales)) || after_bytes > route_limit) {
            release_hlod_bundle(canonical);
            release_hlod_bundle(relative);
            state.hlod_reason = "hlod_candidate_readiness_or_memory_failed";
            if (publish_hlod_bundle(false)) { return 1; }
            return 0;
        }
        // Only now replace complete old bundles. No candidate wrote any old RID.
        release_hlod_resources();
        state.hlod_canonical = std::move(canonical);
        state.hlod_relative = std::move(relative);
        state.hlod_streaming = streaming;
        state.hlod_stream_atlas_records = stream_records;
        state.hlod_streamer = streamer;
        if (served->has_edits()) { ++state.hlod_edit_rebuilds; }
        state.hlod_prepared_snapshot = input;
        state.hlod_instance_generation = instance_generation;
        state.hlod_instance_asset_generation = instance_asset_generation;
        state.hlod_instance_rows = row_count;
        state.hlod_instance_region = instance_payload.region;
        state.hlod_instance_row_data = rows;
        state.hlod_instance_row_keys = row_keys;
        state.hlod_relative_capacity = relative_capacity;
        state.hlod_canonical_capacity = canonical_capacity;
        ++state.hlod_bundle_builds;
        state.hlod_grading_generation = grading_generation;
        state.hlod_prepared_owner_key = owner_key;
        state.hlod_position_bits = position_bits;
        state.hlod_scale_bits = scale_bits;
        state.hlod_quantize_scales = quantize_scales;
    }
    if (!publish_hlod_bundle(false)) { return 0; } // Recheck after allocation before publication.
    state.hlod_frame_active = false;
    state.hlod_cpu_prepare_usec = 0;
    state.hlod_reason = "prepared_awaiting_view_admission";
    r_reason = String();
    return 1;
}


bool GaussianSplatRenderer::prepare_hlod_frame(const RenderFrameContext &p_context) {
    auto &state = get_resource_state();
    state.hlod_frame_active = false;
    state.hlod_selected_nodes = state.hlod_selected_parents = state.hlod_visited_nodes = 0;
    state.hlod_selected_splats = state.hlod_cpu_cut_usec = 0;
    state.hlod_cpu_prepare_usec = 0;
    state.hlod_estimated_records = 0.0;
    if (state.hlod_prepared_snapshot.is_null()) { return false; }
    // PR1b: this view's instance records exist only once the budget rule ran for it (below); until then a fallback idles
    // with no record at all, exactly as before.
    state.hlod_instance_draws.clear();
    bool instance_draws_ready = false;
    uint64_t instance_draws_origin_revision = 0;
    // The objects' part of this main view's shadow caster record (renderer/hlod_shadow_caster_plan.h), formed with the
    // instance records; published with the list that draws them, the world's cut or (idle streamed world) the objects alone.
    gs_shadow_caster::FrameRecord instance_casters;
    bool instance_casters_ready = false;
    auto publish_shadow_casters = [&](uint32_t p_world_records, uint64_t p_world_record_splats) {
        gs_shadow_caster::FrameRecord &caster = state.hlod_shadow_caster_record;
        caster = instance_casters;
        caster.max_chunk_splats = state.hlod_relative.buffers.max_chunk_splats;
        caster.renderer_frame = get_frame_state().frame_counter;
        caster.bundle_builds = state.hlod_bundle_builds;
        caster.draw_list_upload = state.hlod_draw_list_uploads;
        gs_shadow_caster::complete_record(caster, p_world_records, p_world_record_splats);
    };
    if (p_context.pass_kind == RenderPassKind::MAIN_VIEW) {
        // The per-view budget outcome describes THIS main view only: a view that never reaches the budget rule refuses
        // nothing and draws no object, so no earlier view's refusal survives into it.
        state.hlod_world_draw_records = state.hlod_instance_draw_records = 0;
        state.clear_hlod_level_drawn();
        state.hlod_instance_visible_objects = state.hlod_instance_admitted_objects = state.hlod_instance_refused_objects = 0;
        state.hlod_instance_reserve_splats = 0;
        state.hlod_instance_reserve_records = 0.0;
        state.hlod_instance_view_frame = get_frame_state().frame_counter;
        // Shadow casters draw only a list an ACTIVE main view uploaded; an idle or refused view rewrites or abandons it.
        state.hlod_shadow_caster_record = gs_shadow_caster::FrameRecord();
        MutexLock refusal_lock(state.hlod_instance_refusal_mutex);
        state.hlod_instance_refused_keys.clear();
    }
    // Includes admission, scratch readiness, selection and draw-list upload on
    // every return. This CPU elapsed duration is distinct from selection-only
    // hlod_cpu_cut_usec and does not measure GPU execution or bundle creation.
    struct PrepareCpuTimer {
        uint64_t &elapsed;
        uint64_t start = OS::get_singleton()->get_ticks_usec();
        ~PrepareCpuTimer() { elapsed = OS::get_singleton()->get_ticks_usec() - start; }
    } prepare_timer{ state.hlod_cpu_prepare_usec };
    if (!publish_hlod_bundle(false)) { return false; }
    const Ref<GaussianSplatHlodRuntimeData> input = state.hlod_prepared_snapshot;
    // A streamed world has no canonical copy to fall back to: every non-active frame draws nothing (explicitly,
    // with the reason) instead of a partial cut or a whole-world copy.
    auto fallback = [&](const char *p_reason) {
        state.hlod_reason = p_reason;
        // Only a main/camera view that the route refused counts: shadow and manual passes are routine early exits, and so
        // is a deliberate LOD-off of a world that draws its canonical leaves instead. A paged world with LOD off draws
        // nothing (its reason is hlod_paged_lod_disabled), so it does count.
        if (p_context.pass_kind == RenderPassKind::MAIN_VIEW && String(p_reason) != "lod_disabled_canonical_leaves") {
            state.hlod_last_refusal_reason = p_reason;
            ++state.hlod_refused_views;
        }
        if (hlod_edit_committed_here() && !state.hlod_reason.begins_with("hlod_edit_")) {
            state.hlod_reason = String("hlod_edit_route_unavailable:") + p_reason; // ADR 14.5.1: the reason names the cause.
        }
        // ADR 4.4: an idle streamed world still draws the objects of this view (world records = 0).
        if (state.hlod_streaming) {
            // A streamed or paged world has no canonical copy, so a main view the route does not serve (orthographic,
            // multi-view/XR, painterly) or a domain that started after the submission would show no world splats. The
            // automatic route demotes the world to the route it had without it (lod/hlod_auto_route.h); this frame still
            // draws only the objects. Lifting these views on the route itself is follow-up F5.
            gs_hlod_auto_route::RefusalInputs refusal;
            refusal.stage = gs_hlod_auto_route::RefusalStage::FRAME;
            refusal.reason = p_reason;
            refusal.main_view = p_context.pass_kind == RenderPassKind::MAIN_VIEW;
            refusal.no_canonical_copy = true;
            refusal.committed_edit = state.hlod_committed_edit.is_valid();
            request_hlod_auto_route_demotion(refusal);
            const bool with_instances = instance_draws_ready && !state.hlod_instance_draws.is_empty();
            if (idle_hlod_streaming_bundle(with_instances, instance_draws_origin_revision) && with_instances &&
                    p_context.pass_kind == RenderPassKind::MAIN_VIEW) {
                state.hlod_instance_draw_records = state.hlod_instance_draws.size();
                // The objects this view still draws keep casting: their records are the whole uploaded list.
                if (instance_casters_ready) { publish_shadow_casters(0u, 0u); }
            }
        }
        return false;
    };
    // Public LOD-off means the current complete canonical leaf representation.
    // Keep ordinary per-splat culling and budgets; never select coarse parents.
    if (!get_lod_enabled()) { return fallback(input->is_paged() ? hlod_paged_reason_key("lod_disabled_canonical_leaves", false) : "lod_disabled_canonical_leaves"); }
    if (p_context.pass_kind != RenderPassKind::MAIN_VIEW || !p_context.render_data || !p_context.render_buffers ||
            p_context.render_buffers->get_view_count() != 1 || p_context.projection.is_orthogonal() || p_context.painterly_enabled ||
            p_context.viewport_size.x <= 0 || p_context.viewport_size.y <= 0) {
        return fallback("unsupported_view_or_material");
    }
    ProjectSettings *ps = ProjectSettings::get_singleton();
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    // DESIGN E5: a lost world submission used to be reported as deformation_or_instance_domain. It names itself now;
    // wind and sphere effectors keep their own (unchanged) key.
    if (!director || !director->has_hlod_world_submission(this, state.hlod_owner_key)) {
        return fallback(input->is_paged() ? "hlod_paged_world_submission_lost" : "hlod_world_submission_lost");
    }
    if (gs::settings::get_wind_settings(ps).enabled || gs::settings::get_sphere_effector_settings(ps, true).enabled ||
            director->get_sphere_effector_count_for_renderer(this) != 0) { return fallback(input->is_paged() ? hlod_paged_reason_key("deformation_or_instance_domain", false) : "deformation_or_instance_domain"); }
    Ref<TileRenderer> tile = tile_renderer_state.renderer;
    if (tile.is_null() || tile->get_active_device() != get_device_state().rd) { return fallback("tile_device_unavailable"); }
    const uint64_t route_limit = hlod_route_limit_bytes(state, get_device_state().rd);
    uint64_t route_bytes = 0;
    if (!get_hlod_owned_byte_upper_bound(route_bytes)) { return fallback("resource_ledger_unavailable"); }
    state.device_vram_ledger.admit_device_bytes(route_bytes);
    // The frame's scratch request (admitted by prepare_hlod_scratch below). The scratch covers the atlas the frame draws
    // from (its total_gaussians), not the tree's splat count; PR1b: plus the instance region at the atlas tail.
    const uint32_t total = ResidentAtlasBudget::hlod_relative_atlas_records(state.hlod_streaming, state.hlod_stream_atlas_records,
            input->leaf_count, input->parent_count) + uint32_t(state.hlod_instance_region.records);
    const PipelineFeatureSet *features = p_context.deps.pipeline_features;
    if (!features) { return fallback("current_pipeline_features_unavailable"); }
    TileRenderer::RenderParams scratch_params;
    scratch_params.viewport_size = p_context.viewport_size;
    scratch_params.total_gaussians = total;
    scratch_params.max_visible_splats = state.hlod_relative.buffers.max_visible_splats;
    scratch_params.request_packed_stage_data = features->enable_packed_stage_data;
    scratch_params.request_tighter_bounds = features->enable_tighter_bounds;
    scratch_params.request_sh_amortization = features->enable_sh_amortization;
    scratch_params.sh_amortization_divisor = features->sh_amortization_divisor;
    // The ledger order of this frame (ResidentAtlasBudget::hlod_optional_growth_headroom). Every step below allocates from
    // route_limit - route_bytes. The world's mandatory allocations still outstanding are set aside first: the view's tile
    // grid (streamed and paged worlds), the frame scratch and the ViewSet reservation of the views. The optional overlap
    // growths (measured overflow, objects' estimate) may only spend the rest, so they can never leave the scratch, the grid
    // or a view refused on every frame; refused, they only leave the storage as it is.
    uint64_t mandatory_bytes = 0;
    {
        uint64_t grid_bytes = 0;
        if (state.hlod_streaming || input->is_paged()) {
            uint64_t live_textures = 0;
            if (!tile->get_hlod_texture_byte_upper_bound(live_textures)) { return fallback("resource_ledger_unavailable"); }
            const Vector2i grid = tile->get_provisioned_viewport();
            const uint32_t overlap_now = tile->get_allocated_overlap_capacity();
            const uint32_t seed = overlap_now == 0 ? tile->clamp_hlod_overlap_records(uint32_t(MIN<uint64_t>(uint64_t(input->leaf_count) * 50u, UINT32_MAX))) : 0u;
            grid_bytes = ResidentAtlasBudget::hlod_view_grid_outstanding_bytes(grid.x, grid.y, p_context.viewport_size.x, p_context.viewport_size.y,
                    overlap_now, live_textures,
                    tile->estimate_hlod_texture_bytes_for_viewport(p_context.viewport_size) + uint64_t(seed) * tile->get_hlod_overlap_bytes_per_record());
        }
        // Each admitted view (and this one, if it is new) is charged its history and draw records against the same
        // headroom every frame (ViewSet::admit_view / select): node_budget = the world's nodes, one world instance.
        const uint32_t views = MIN<uint32_t>(frame_context_manager.get_hlod_view_set().view_count() + 1u, RenderFrameContextManager::HLOD_MAX_VIEWS);
        mandatory_bytes = ResidentAtlasBudget::hlod_mandatory_outstanding_bytes(grid_bytes, tile->get_hlod_scratch_outstanding_bytes(scratch_params),
                hlod_view_reservation_bytes(input->nodes.size(), views));
    }
    // Safe point for the overlap storage: the previous frames' measured overlap demand, before this frame's passes. It runs
    // before the per-view grid decision below, so that decision (and the seed) reads the grown capacity. Optional: only
    // from the headroom the mandatory allocations leave (an over-limit route grows nothing and is refused below).
    if (tile->grow_hlod_overlap_storage(ResidentAtlasBudget::hlod_optional_growth_headroom(route_limit, route_bytes, mandatory_bytes))) {
        if (!get_hlod_owned_byte_upper_bound(route_bytes)) { return fallback("resource_ledger_unavailable"); }
        state.device_vram_ledger.admit_device_bytes(route_bytes);
    }
    // PR1b: this view's visible node instances and their demand (ADR 4.5), computed here, at the same safe point, from
    // the same camera, frustum, focal length and tile area the world's ViewSpec uses below. Their overlap-record estimate
    // is held NEXT TO the world's: the storage grows to the world's seed rule plus the objects' estimate (priced against
    // the route's headroom; clamped like the seed), because it otherwise only grows on a measured overflow and a world cut
    // pinned coarse by the objects' reserve never overflows. A refused growth leaves the split below to decide.
    LocalVector<gs_hlod_bundle::ObjectDemand> demands;
    // The world's own record need: the storage it had before any growth for objects, or its seed rule if larger.
    double world_record_need = double(tile->get_allocated_overlap_capacity());
    {
        const gs_hlod_bundle::Rows &rows = state.hlod_instance_row_data;
        const gs_hlod_bundle::InstanceRegion &region = state.hlod_instance_region;
        if (rows.count() != state.hlod_instance_rows || state.hlod_instance_row_keys.size() != rows.count()) {
            return fallback("hlod_instance_rows_inconsistent");
        }
        const Transform3D view_to_world = p_context.world_to_camera_transform.affine_inverse();
        Plane view_frustum[6];
        for (uint32_t i = 0; i < 6; ++i) {
            view_frustum[i] = view_to_world.xform(p_context.cull_projection.get_projection_plane(Projection::Planes(i)));
        }
        const double view_focal = std::abs(double(p_context.cull_projection.columns[1][1])) * p_context.viewport_size.y * 0.5;
        const double view_tile_area = double(tile->get_tile_size()) * tile->get_tile_size();
        double demand_records = 0.0;
        for (uint32_t row = 1; row < rows.count(); ++row) {
            const int asset_index = region.find(rows.asset_ids[row]);
            if (asset_index < 0) { return fallback("hlod_instance_rows_inconsistent"); }
            const gs_hlod_bundle::InstanceAsset &asset = region.assets[asset_index];
            const InstanceDataGPU &r = rows.instances[row];
            const float scale = std::fabs(r.translation_scale[3]);
            const Vector3 translation(r.translation_scale[0], r.translation_scale[1], r.translation_scale[2]);
            const Quaternion rotation(r.rotation[0], r.rotation[1], r.rotation[2], r.rotation[3]);
            const Vector3 center = rotation.xform(asset.bounds_center * scale) + translation;
            gs_hlod_bundle::ObjectDemand demand;
            if (!gs_hlod_bundle::object_view_demand(asset, center, scale, view_frustum, get_frustum_culling(), view_to_world.origin,
                        view_focal, view_tile_area, demand.importance, demand.records)) {
                continue; // Outside this view: it reserves nothing and is not refused.
            }
            demand.instance_key = state.hlod_instance_row_keys[row];
            demand.row = row;
            demand.asset_index = uint32_t(asset_index);
            demand.splats = asset.splat_count;
            demand.chunks = asset.chunk_count;
            demands.push_back(demand);
            if (std::isfinite(demand.records)) { demand_records += demand.records; }
        }
        // The world's need is tracked apart from the storage (gs_hlod_bundle::world_overlap_need): the storage already
        // holds the objects' estimate of earlier frames, so it is not the world's need, and the requirement is absolute
        // (world need + this view's estimate, once). Unchanged content never grows it again.
        const uint64_t world_need = gs_hlod_bundle::world_overlap_need(state.hlod_overlap_world_need, tile->get_allocated_overlap_capacity(),
                demands.is_empty() ? 0u : tile->clamp_hlod_overlap_records(uint32_t(MIN<uint64_t>(uint64_t(input->leaf_count) * 50u, UINT32_MAX))));
        world_record_need = double(world_need);
        if (!demands.is_empty()) {
            const uint32_t wanted = gs_hlod_bundle::world_plus_objects_overlap_records(world_need, demand_records);
            // Optional: only what the outstanding mandatory allocations leave (see the ledger order above).
            const uint64_t headroom = ResidentAtlasBudget::hlod_optional_growth_headroom(route_limit, route_bytes, mandatory_bytes);
            // Grow with slack (moving objects must not reallocate it on every small rise), else exactly to the requirement.
            if (tile->clamp_hlod_overlap_records(wanted) > tile->get_allocated_overlap_capacity() &&
                    (tile->ensure_hlod_overlap_capacity(gs_hlod_bundle::world_plus_objects_overlap_growth(world_need, demand_records), headroom) ||
                            tile->ensure_hlod_overlap_capacity(wanted, headroom))) {
                ++state.hlod_instance_overlap_growths;
                if (!get_hlod_owned_byte_upper_bound(route_bytes)) { return fallback("resource_ledger_unavailable"); }
                state.device_vram_ledger.admit_device_bytes(route_bytes);
            }
        }
        state.hlod_overlap_world_need.world_records = world_need;
        state.hlod_overlap_world_need.storage_records = tile->get_allocated_overlap_capacity();
    }
    if (state.hlod_streaming || input->is_paged()) {
        // The one tile-grid provisioner for streamed (D1) and V4 paged (E1) worlds. Neither runs a canonical frame after
        // startup, and prepare_hlod_scratch() refuses every frame whose size differs from the tile grid. Resize the grid to
        // THIS view before admitting its scratch, per view and per frame (two views of different sizes alternate here).
        // provision_paged_hlod_route() keeps only the compositor scratch and the first overlap seed. This runs BEFORE the
        // route limit refusal: a smaller view must be able to shrink a grid an earlier, larger view pushed over the limit,
        // or the route would stay refused for good.
        const Vector2i view_size = p_context.viewport_size;
        const Vector2i grid = tile->get_provisioned_viewport();
        uint64_t live_textures = 0;
        if (!tile->get_hlod_texture_byte_upper_bound(live_textures)) { return fallback("resource_ledger_unavailable"); }
        const uint32_t overlap_now = tile->get_allocated_overlap_capacity();
        // Seed the overlap storage only when no canonical warmup ever created it (every leaf may be visible, 50 records each;
        // the sorting config caps it); otherwise the resize keeps the capacity and rebuilds the grid-sized buffers.
        // It is capped at the configured max_overlap_records (as the allocation is) and priced at the real bytes per record.
        const uint32_t seed = overlap_now == 0 ? tile->clamp_hlod_overlap_records(uint32_t(MIN<uint64_t>(uint64_t(input->leaf_count) * 50u, UINT32_MAX))) : 0u;
        const uint64_t seed_bytes = uint64_t(seed) * tile->get_hlod_overlap_bytes_per_record();
        const uint64_t wanted_bytes = tile->estimate_hlod_texture_bytes_for_viewport(view_size) + seed_bytes;
        const auto action = ResidentAtlasBudget::hlod_view_grid_action(grid.x, grid.y, view_size.x, view_size.y, overlap_now, live_textures,
                wanted_bytes, state.device_vram_ledger.headroom());
        if (action == ResidentAtlasBudget::HlodViewGridAction::REFUSE_MEMORY) { return fallback("hlod_view_size_memory_limit"); }
        if (action == ResidentAtlasBudget::HlodViewGridAction::RESIZE) {
            if (tile->provision_hlod_route(view_size, RD::DATA_FORMAT_MAX, seed) != OK) { return fallback("hlod_view_provisioning_failed"); }
            if (!get_hlod_owned_byte_upper_bound(route_bytes)) { return fallback("resource_ledger_unavailable"); }
            state.device_vram_ledger.admit_device_bytes(route_bytes);
            // Over the limit after the resize: refuse this view. The targets stay, but the next smaller view shrinks them above.
            if (route_bytes > route_limit) { return fallback("hlod_view_size_memory_limit"); }
        }
    }
    if (route_bytes > route_limit) { return fallback("route_memory_limit"); }
    // Mandatory: the frame scratch, from the headroom the optional growths above had to leave for it.
    if (!tile->prepare_hlod_scratch(scratch_params,
                route_limit - route_bytes)) { return fallback("tile_capacity_warmup_or_memory_limit"); }
    if (!get_hlod_owned_byte_upper_bound(route_bytes) || route_bytes > route_limit) { return fallback("prepared_route_memory_limit"); }
    state.device_vram_ledger.admit_device_bytes(route_bytes);
    {
        uint64_t ledger_textures = 0;
        if (!tile->get_hlod_texture_byte_upper_bound(ledger_textures)) { ledger_textures = 0; } // Attribution only.
        report_hlod_ledger_categories(state, *input.ptr(), *tile.ptr(), ledger_textures, subsystem_state.sorting_pipeline->get_buffer_handles(),
                splat_shadow_caster ? splat_shadow_caster->get_owned_buffer_bytes() : 0u);
    }
    const uint32_t records = tile->get_allocated_overlap_capacity();
    if (records == 0) { return fallback("overlap_capacity_unavailable"); }
    state.hlod_overlap_capacity_records = records;
    const double tau = double(state.hlod_tau_px > 0 ? state.hlod_tau_px : gs::settings::get_hlod_tau_px(ps)) * get_lod_bias();
    if (!std::isfinite(tau) || tau <= 0.0) { return fallback("invalid_tau"); }
    // The renderer no longer keeps histories of its own: gs_hlod_runtime::ViewSet owns every per-view cut,
    // keyed by the RenderSceneBuffersRD id and lifetime epoch (RenderFrameContextManager::select_hlod_view).
    // Policy (tau, tile area, caps) invalidation lives in ViewSet; the single-asset scope is one instance.
    HlodViewIdentity identity;
    if (!RenderFrameContextManager::acquire_hlod_identity(p_context.render_buffers, identity)) {
        state.hlod_view_epoch = 0;
        return fallback("complete_view_history_unavailable");
    }
    state.hlod_view_epoch = identity.epoch;
    using gs_hlod_runtime::SelectionSnapshot;
    SelectionSnapshot &snapshot = frame_context_manager.get_hlod_snapshot();
    struct SnapshotRelease { RenderFrameContextManager &manager; ~SnapshotRelease() { manager.release_hlod_snapshot_data(); } } snapshot_release{ frame_context_manager };
    gs_hlod_runtime::ViewSpec spec;
    const Transform3D camera_to_world = p_context.world_to_camera_transform.affine_inverse();
    spec.camera = camera_to_world.origin;
    // Godot only provides a float camera transform here, so this is origin + float camera (double add): precise
    // only as far as that float, hence the contract that the origin stays near the camera (see ViewSpec).
    // `camera_precise` is filled once the snapshot origin is known, below.
    spec.frustum_enabled = get_frustum_culling();
    for (uint32_t i = 0; i < 6; ++i) {
        spec.frustum[i] = camera_to_world.xform(p_context.cull_projection.get_projection_plane(Projection::Planes(i)));
    }
    spec.focal_pixels = std::abs(double(p_context.cull_projection.columns[1][1])) * p_context.viewport_size.y * 0.5;
    spec.tau_pixels = tau;
    if (state.hlod_streaming) {
        // Plan the streamer's demand in atlas pages (ancestors of the cut included), not in splats.
        spec.page_splats = HlodNodeStreamer::Config::DEFAULT_PAGE_SPLATS;
        spec.page_budget = state.hlod_stream_atlas_records / spec.page_splats;
    }
    spec.tile_area = double(tile->get_tile_size()) * tile->get_tile_size();
    // The drawn set can never exceed what the atlas holds (ADR 6.5: the effective budget is the minimum of the
    // configured cap and the allocated capacity). A streamed atlas is smaller than the cut cap, so a refinement
    // that could not be resident is refused by the selector instead of being requested and evicted again.
    //
    // PR1b, the budget rule (ADR 4.5; renderer/hlod_world_instances.h). The view's performance caps are the view splat cap
    // C_view and the overlap-record cap. The world keeps the floor F of each; the visible objects reserve their
    // need from the rest FIRST, in a deterministic order (screen importance, ties by the stable instance key), and an
    // object that does not fit is refused whole, with a reason and a counter, never drawn partially. The world cut then
    // gets the caps minus the reserve, never less than F. The draw list holds the world's nodes plus every row's chunks,
    // so it never refuses an object. Without node instances the caps are exactly what they were.
    // C_view: the view's performance cap the objects reserve from (not the world's own data-count-clamped max_splats).
    const uint64_t splat_cap = MAX<uint64_t>(get_hlod_view_splat_cap(),
            gs_hlod_bundle::world_need_splats(input->leaf_count, get_performance_settings().max_splats));
    const uint32_t world_nodes = input->nodes.size();
    gs_hlod_bundle::Admission admission;
    {
        const gs_hlod_bundle::InstanceRegion &region = state.hlod_instance_region;
        gs_hlod_bundle::ObjectShare share;
        share.splats = gs_hlod_bundle::object_share_splats(splat_cap);
        share.nodes = region.row_chunks;
        share.records = gs_hlod_bundle::object_share_records(double(tile->clamp_hlod_overlap_records(UINT32_MAX)), double(records),
                world_record_need);
        gs_hlod_bundle::admit_objects(demands.ptr(), demands.size(), share, admission);
    }
    state.hlod_instance_visible_objects = demands.size();
    state.hlod_instance_admitted_objects = admission.admitted.size();
    state.hlod_instance_refused_objects = admission.refused.size();
    state.hlod_instance_reserve_splats = admission.splats;
    state.hlod_instance_reserve_records = admission.records;
    state.hlod_instance_view_frame = get_frame_state().frame_counter;
    if (!admission.refused.is_empty()) {
        ++state.hlod_instance_reserve_refusal_frames;
        WARN_PRINT_ONCE(vformat("[GaussianSplatRenderer] HLOD route: %d object(s) refused (%s): their reserve exceeds the object share "
                                "of the view caps; the world keeps its floor. See hlod_instance_refused_objects.",
                admission.refused.size(), gs_hlod_bundle::INSTANCE_REFUSAL_REASON));
    }
    {
        MutexLock refusal_lock(state.hlod_instance_refusal_mutex);
        state.hlod_instance_refused_keys.clear();
        for (uint32_t index : admission.refused) { state.hlod_instance_refused_keys.push_back(demands[index].instance_key); }
    }
    gs_hlod_bundle::world_view_caps(input->leaf_count, get_performance_settings().max_splats, splat_cap, state.hlod_streaming, state.hlod_stream_atlas_records, double(records),
            admission, spec.caps.splat_budget, spec.caps.record_budget);
    spec.caps.node_budget = world_nodes;
    if (state.hlod_streaming && state.hlod_streamer.is_valid()) {
        // Package F1: the streamer checks a staged edit's replacement cut against the caps of the views it knows.
        gs_hlod_runtime::CutRequest known_view;
        known_view.camera = spec.camera;
        for (uint32_t i = 0; i < 6; ++i) { known_view.frustum[i] = spec.frustum[i]; }
        known_view.frustum_enabled = spec.frustum_enabled;
        known_view.focal_pixels = spec.focal_pixels;
        known_view.tau_pixels = spec.tau_pixels;
        known_view.record_budget = spec.caps.record_budget;
        known_view.tile_area = spec.tile_area;
        known_view.splat_budget = spec.caps.splat_budget;
        known_view.node_budget = spec.caps.node_budget;
        known_view.page_splats = spec.page_splats;
        known_view.page_budget = spec.page_budget;
        // Keyed by the view's real HLOD identity: a constant key made the second viewport overwrite the first.
        register_hlod_known_view(identity, known_view);
    }
    // One asset, one identity-placed instance holding the whole view's caps. The GPU-side sort/record/draw
    // buffers and the payload are already inside the route ledger above (route_bytes), so the ViewSet
    // budget is the remaining headroom and charges only what it allocates on the CPU (histories, draw records).
    snapshot.hierarchy_revision = input->source_revision;
    // The render-frame origin and its revision come from the module's origin context (the director), so a shift
    // invalidates this view's cut in ViewSet::select(). The GPU consumes the per-draw camera-relative offset of each
    // record (DrawnNodeGPU), formed in double against this origin; every frame-dependent cache of the route is keyed
    // on the same revision (hlod_origin_revision).
    //
    // The fallback below stays until the end-of-stack GPU gate has captured a far-origin near-detail frame, a
    // multi-view frame and an origin shift on a real device. The CPU side (offsets in double, record layout, keys,
    // the GLSL camera-relative function against a double reference) is tested, but the shifted-origin GPU frame is
    // not: this route never draws a frame it could not verify. Removing the guard is this one line.
    snapshot.origin = gs_world_frame::PreciseCoord();
    snapshot.origin_revision = 0;
    if (const GaussianSplatSceneDirector *origin_director = GaussianSplatSceneDirector::get_singleton()) {
        origin_director->get_world_origin(snapshot.origin, snapshot.origin_revision);
    }
    if (snapshot.origin != gs_world_frame::PreciseCoord()) { return fallback("hlod_world_origin_shifted"); }
    {
        const double camera_delta[3] = { double(spec.camera.x), double(spec.camera.y), double(spec.camera.z) };
        spec.camera_precise_valid = snapshot.origin.translated(camera_delta, spec.camera_precise);
    }
    // PR1b: the admitted objects' draw records, one per (row, chunk), after the world's. The payload is asset-local, so the
    // offset is T - camera, formed in double against the same origin (and revision) as the world's offsets.
    for (uint32_t index : admission.admitted) {
        const gs_hlod_bundle::ObjectDemand &demand = demands[index];
        if (!spec.camera_precise_valid) { return fallback("hlod_instance_camera_frame_invalid"); }
        const InstanceDataGPU &r = state.hlod_instance_row_data.instances[demand.row];
        Vector3 offset;
        if (!gs_hlod_bundle::instance_camera_offset(Vector3(r.translation_scale[0], r.translation_scale[1], r.translation_scale[2]),
                    snapshot.origin, spec.camera_precise, offset)) {
            return fallback("hlod_instance_offset_invalid");
        }
        gs_hlod_bundle::append_instance_draws(state.hlod_instance_region, world_nodes, demand.row, demand.asset_index, offset,
                state.hlod_instance_draws);
    }
    instance_draws_ready = true;
    instance_draws_origin_revision = snapshot.origin_revision;
    {
        // Shadow casters: which rows cast (row 0 = the world's own cast_shadow), and the admitted objects' splats among them.
        LocalVector<uint64_t> caster_keys;
        const bool world_casts = director->collect_hlod_shadow_caster_keys_for_renderer(this, caster_keys);
        gs_shadow_caster::build_row_casts(state.hlod_instance_row_keys, world_casts, caster_keys, instance_casters.row_casts);
        instance_casters.instance_records = state.hlod_instance_draws.size();
        instance_casters.instance_splats = gs_shadow_caster::admitted_instance_splats(state.hlod_instance_region, demands.ptr(),
                demands.size(), admission, instance_casters.row_casts);
        instance_casters.quantized = g_quantization_config.per_chunk_quantization;
        instance_casters.camera = spec.camera; // The objects' offsets were formed against it (above).
        instance_casters.origin_revision = snapshot.origin_revision;
        instance_casters_ready = true;
    }
#ifdef TESTS_ENABLED
    // Test-only: a streamed world held idle, with this view's objects formed (the state a loading cut is in).
    if (state.hlod_streaming && state.hlod_test_hold_world_idle.load(std::memory_order_acquire)) {
        return fallback("hlod_test_world_held_idle");
    }
#endif
    snapshot.budget = gs_hlod_runtime::ResidencyBudget();
    snapshot.budget.physical_bytes = state.device_vram_ledger.headroom();
    snapshot.assets.resize(1);
    snapshot.instances.resize(1);
    gs_hlod_runtime::SnapshotAsset &asset = snapshot.assets.write[0];
    asset.asset_key = state.hlod_prepared_owner_key;
    asset.data = input;
    asset.bake_rule_revision = 0;
    // Whole-tree bundle: everything it holds is resident. Streamed bundle: only nodes the streamer has validated
    // and completely uploaded (resident-ready) are visible to the selector, so a cut can only name drawable nodes.
    Ref<GaussianSplatHlodRuntimeData> served = input;
    if (state.hlod_streaming) {
        // The publication boundary of a staged edit: commit, adopt, then select, structurally (one function, tested).
        served = hlod_stream_commit_boundary();
        if (served.is_null()) {
            // A failed commit part-way: the bundle cannot be trusted. Release; the rebuild serves the last COMMITTED
            // snapshot (hlod_committed_edit), and until then the world draws nothing (hlod_edit_blanks_frame).
            state.hlod_edit_event = "hlod_edit_commit_unusable";
            release_hlod_resources();
            state.hlod_reason = "hlod_edit_commit_unusable";
            return false;
        }
        asset.data = served;
        asset.resident = state.hlod_streamer->resident_mask();
    } else {
        asset.resident.clear();
    }
    asset.payload_bytes = 0;
    gs_hlod_runtime::SnapshotInstance &instance = snapshot.instances.write[0];
    instance.instance_key = 1;
    instance.asset_index = 0;
    instance.placement = gs_world_frame::PrecisePlacement(); // World == asset frame (the tree origin is double).
    instance.caps = spec.caps;
    const uint64_t start = OS::get_singleton()->get_ticks_usec();
    const HlodViewOutcome outcome = frame_context_manager.select_hlod_view(identity, input->get_instance_id(), snapshot, spec);
    state.hlod_cpu_cut_usec = OS::get_singleton()->get_ticks_usec() - start;
    if (state.hlod_streaming) {
        // Desired(view) is demand only: it requests, it never makes anything drawable. It is available while the
        // view is still loading, which is exactly when the coarse frontier has to be requested.
        uint32_t desired_count = 0;
        const gs_hlod_runtime::DesiredRecord *desired = frame_context_manager.get_hlod_view_set().desired(
                RenderFrameContextManager::hlod_view_key(identity), desired_count);
        HlodNodeStreamer::Buffers stream_buffers;
        stream_buffers.atlas = state.hlod_relative.buffers.atlas_gaussian_buffer;
        stream_buffers.chunk_meta = state.hlod_relative.buffers.chunk_meta_buffer;
        stream_buffers.quantization = state.hlod_relative.buffers.quantization_buffer;
        state.hlod_streamer->update(get_device_state().rd, stream_buffers, desired, desired_count,
                outcome.active() ? outcome.records : nullptr, outcome.active() ? outcome.record_count : 0u);
        state.hlod_edit_event = state.hlod_streamer->get_telemetry().edit_event;
#ifdef TESTS_ENABLED
        hlod_edit_probe_step();
#endif
        ++state.hlod_stream_frames;
        if (outcome.active()) {
            ++state.hlod_stream_ready_frames;
            if (state.hlod_stream_first_ready_frame == 0) { state.hlod_stream_first_ready_frame = uint32_t(state.hlod_stream_frames); }
        } else {
            ++state.hlod_stream_loading_frames;
        }
    }
    // Every non-success readiness or admission result maps to the canonical non-HLOD route with an explicit
    // reason; an active cut is only ever a READY (complete) view, never an empty or partial one.
    if (!outcome.active()) {
        if (served->has_edits() && outcome.view.edit_forced) { ++state.hlod_edit_forced_frames; }
        if (served->has_edits() && state.hlod_streaming && !state.hlod_streamer->root_failed() &&
                outcome.view.readiness != gs_hlod_runtime::ViewSet::FAILED && outcome.view.edit_forced) {
            // The replacement cut of an edited world is not drawable yet: say so with a reason of its own (P2-3).
            const HlodNodeStreamer::Telemetry &edit_t = state.hlod_streamer->get_telemetry();
            if (outcome.view.readiness == gs_hlod_runtime::ViewSet::LOADING && edit_t.stall_frames < 60) {
                return fallback("hlod_edit_replacement_loading");
            }
            return fallback("hlod_edit_replacement_not_admitted");
        }
        state.hlod_budget_bound_frames += outcome.fallback.budget_bound_frames;
        state.hlod_root_dropped_frames += outcome.fallback.root_dropped_frames;
        if (outcome.fallback.root_dropped_frames) {
            WARN_PRINT_ONCE("HLOD found no complete frontier within the admission caps; using the canonical route. See hlod_root_dropped_frames and hlod_reason.");
        }
        if (state.hlod_streaming) {
            // Loading, not admitted and failed are different states (ADR 14.2); a stalled or failed stream
            // must not read as an ordinary load.
            const HlodNodeStreamer::Telemetry &t = state.hlod_streamer->get_telemetry();
            if (state.hlod_streamer->root_failed()) { return fallback("hlod_stream_root_load_failed"); }
            if (outcome.view.readiness == gs_hlod_runtime::ViewSet::LOADING) {
                if (t.stall_frames < 60) { return fallback("hlod_stream_loading"); }
                // Stalled: say why. A failed child is a load failure (ADR 14.2), enough free pages without a
                // contiguous run is fragmentation (compaction, D2), anything else is the budget.
                if (t.blocked_groups_this_frame > 0) { return fallback("hlod_stream_node_load_failed"); }
                if (t.fragmented_groups_this_frame > 0) { return fallback("hlod_stream_stalled_atlas_fragmentation"); }
                return fallback("hlod_stream_not_admitted_atlas_budget");
            }
        }
        return fallback(outcome.fallback.reason);
    }
    auto &bundle = state.hlod_relative;
    // Draw records are ordered by NodeKey, not by selection order. The GPU treats the list as a lookup table
    // (depth_compute stores the chunk index per splat, tile_binning reads entries[index]); no consumer depends
    // on entry order, and the splat order itself comes from an atomic append plus the depth sort.
    if (served->has_edits() && outcome.view.edit_forced) { ++state.hlod_edit_forced_frames; } // A drawn cut that replaced a stale ancestor.
    const uint32_t selected_count = outcome.record_count;
    const uint32_t instance_records = state.hlod_instance_draws.size();
    if (selected_count > world_nodes || uint64_t(selected_count) + instance_records > uint64_t(bundle.draw_entries.size())) {
        return fallback("hlod_view_failed");
    }
    if (state.hlod_streaming) {
        // Absence of a signal is not a pass: re-check that every drawn node is resident-ready before its record
        // reaches the GPU. The selector only sees resident nodes, so a miss here is a contract violation.
        for (uint32_t i = 0; i < selected_count; ++i) {
            if (state.hlod_streamer->get_residency().state(outcome.records[i].node) != gs_hlod_runtime::NodeResidency::RESIDENT) {
                ++state.hlod_stream_nonresident_drawn;
                return fallback("hlod_stream_drawn_node_not_resident");
            }
        }
    }
    state.clear_hlod_level_drawn();
    for (uint32_t i = 0; i < selected_count; ++i) {
        const uint32_t node = outcome.records[i].node;
        // The ViewSet formed camera_offset in double for snapshot.origin_revision; every world record names the
        // world's row (row 0, identity-placed). BV-04b: the record carries the node's HLOD level (height) so the
        // emit pass can count records per splat per level.
        const uint32_t level = input->nodes[node].topology.height;
        bundle.draw_entries.write[i] = gs_hlod_gpu::make_drawn_node(gs_hlod_bundle::WORLD_ROW, node, outcome.records[i].camera_offset, level);
        const uint32_t level_slot = gs_hlod_level_stat_slot(level);
        state.hlod_level_drawn_nodes[level_slot]++;
        state.hlod_level_drawn_splats[level_slot] += input->nodes[node].topology.payload_count;
        state.hlod_selected_parents += !input->nodes[node].topology.is_leaf() ? 1u : 0u;
    }
    // The objects' records follow the world's: one draw list, one depth sort (record order is a lookup table, not a draw
    // order; the depth sort orders world and object splats together).
    for (uint32_t k = 0; k < instance_records; ++k) {
        bundle.draw_entries.write[selected_count + k] = state.hlod_instance_draws[k];
    }
    const uint32_t draw_count = selected_count + instance_records;
    RenderingDevice *rd = get_device_state().rd;
    // Upload to the relative candidate only; no canonical RID is modified.
    const uint32_t counters[3] = { draw_count, 0u, 0u };
    if ((draw_count && rd->buffer_update(bundle.buffers.visible_chunk_buffer, 0,
                draw_count * sizeof(DrawnNodeGPU), bundle.draw_entries.ptr()) != OK) ||
            rd->buffer_update(bundle.buffers.counter_buffer, 0, sizeof(counters), counters) != OK) {
        return fallback("draw_list_upload_failed");
    }
    ++state.hlod_draw_list_uploads;
    bundle.buffers.hlod_draw_count = draw_count;
    state.hlod_world_draw_records = selected_count;
    state.hlod_instance_draw_records = instance_records;
    // The uploaded offsets are valid for exactly this revision; publish_hlod_bundle() copies it into the buffers the
    // sort and tile caches key on.
    bundle.buffers.hlod_origin_revision = snapshot.origin_revision;
    state.hlod_origin_revision = snapshot.origin_revision;
    state.hlod_selected_nodes = selected_count;
    state.hlod_selected_splats = outcome.view.selected_splats;
    state.hlod_estimated_records = outcome.view.estimated_records;
    state.hlod_visited_nodes = uint32_t(std::min<uint64_t>(outcome.view.visited_nodes, UINT32_MAX));
    state.hlod_budget_bound_frames += outcome.view.budget_bound ? 1u : 0u;
    if (!publish_hlod_bundle(true)) { return false; }
    state.hlod_frame_active = true;
    state.hlod_reason = "active";
    if (p_context.pass_kind == RenderPassKind::MAIN_VIEW && instance_casters_ready) {
        // The next frame's shadow passes draw exactly this list (renderer/hlod_shadow_caster_plan.h): its offsets were
        // formed against spec.camera, its objects are the admitted ones.
        uint64_t world_record_splats = 0;
        for (uint32_t i = 0; i < selected_count; ++i) {
            world_record_splats += input->nodes[outcome.records[i].node].topology.payload_count;
        }
        publish_shadow_casters(selected_count, world_record_splats);
    }
    return true;
}

bool GaussianSplatRenderer::get_hlod_owned_byte_upper_bound(uint64_t &r_bytes) const {
    // RD buffer_memory is decremented only when _free_pending_resources actually
    // destroys its retired buffer (rendering_device.cpp). This device-wide superset
    // includes shared/common/retired buffers once; it is not process VRAM.
    RenderingDevice *rd = get_device_state().rd;
    if (!rd || tile_renderer_state.renderer.is_null()) { return false; }
    uint64_t textures = 0;
    if (!tile_renderer_state.renderer->get_hlod_texture_byte_upper_bound(textures)) { return false; }
    const OutputCompositor *compositor = subsystem_state.output_compositor.ptr();
    uint64_t scratch = 0;
    if (!compositor || !compositor->get_hlod_scratch_byte_upper_bound(rd, scratch)) { return false; }
    if (scratch > UINT64_MAX - textures) { return false; }
    textures += scratch;
    const uint64_t buffers = rd->get_memory_usage(RD::MEMORY_BUFFERS);
    if (textures > UINT64_MAX - buffers) { return false; }
    r_bytes = buffers + textures;
    return true;
}

#ifdef TESTS_ENABLED
void GaussianSplatRenderer::test_set_hlod_route_limit_override(int64_t p_bytes) {
    get_resource_state().hlod_test_route_limit_override.store(p_bytes > 0 ? uint64_t(p_bytes) : 0u, std::memory_order_release);
}

void GaussianSplatRenderer::test_set_hlod_hold_world_idle(bool p_hold) {
    get_resource_state().hlod_test_hold_world_idle.store(p_hold, std::memory_order_release);
}

void GaussianSplatRenderer::test_set_hlod_publication_failure(int p_failure) {
    ERR_FAIL_COND(p_failure < 0 || p_failure > 5);
    get_resource_state().hlod_test_failure_hits.store(0, std::memory_order_relaxed);
    get_resource_state().hlod_test_failure.store(p_failure, std::memory_order_release);
}

bool GaussianSplatRenderer::test_hlod_publication_refuses(int p_failure) {
    auto &state = get_resource_state();
    if (state.hlod_test_failure.load(std::memory_order_acquire) != p_failure) { return false; }
    state.hlod_test_failure_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void GaussianSplatRenderer::_test_cancel_hlod_publication_observation() {
    Ref<HlodPublicationObservation> observation = test_hlod_observation;
    test_hlod_observation.unref();
    if (observation.is_valid()) { observation->cancel(); }
}

void GaussianSplatRenderer::HlodPublicationObservation::_fail_locked(const String &p_error) {
    if (failed || cancelled) { return; }
    failed = true;
    complete = true;
    result["error"] = p_error;
    result["fatal"] = true;
    result["readback_complete"] = false;
}

void GaussianSplatRenderer::HlodPublicationObservation::fail(const String &p_error) {
    MutexLock lock(mutex);
    _fail_locked(p_error);
}

void GaussianSplatRenderer::HlodPublicationObservation::cancel() {
    MutexLock lock(mutex);
    cancelled = true;
    complete = true;
    result["error"] = "publication_readback_cancelled";
    result["fatal"] = true;
    result["readback_complete"] = false;
}

Callable GaussianSplatRenderer::HlodPublicationObservation::make_callback(
        const Ref<HlodPublicationObservation> &p_observation, int p_slot) {
    if (p_observation.is_null()) { return Callable(); }
    MutexLock lock(p_observation->mutex);
    // Binding the Ref pins only the box until RD releases its queued Callable.
    // No lookup of the releasing renderer/device is needed by a late callback.
    return callable_mp_static(&HlodPublicationObservation::_readback).bind(
            p_observation, int64_t(p_observation->request_id), p_slot, int64_t(p_observation->device_id));
}

void GaussianSplatRenderer::HlodPublicationObservation::_readback(const Vector<uint8_t> &p_bytes,
        const Ref<HlodPublicationObservation> &p_observation, int64_t p_request_id, int p_slot, int64_t p_device_id) {
    if (p_observation.is_null()) { return; }
    MutexLock lock(p_observation->mutex);
    HlodPublicationObservation &observation = *p_observation.ptr();
    if (observation.cancelled || observation.failed || p_request_id <= 0 ||
            observation.request_id != uint64_t(p_request_id) || observation.device_id != uint64_t(p_device_id)) { return; }
    if (p_slot < 0 || p_slot >= 4 || (observation.received_mask & (1u << p_slot)) != 0 ||
            uint64_t(p_bytes.size()) != observation.expected_bytes[p_slot]) {
        observation._fail_locked("publication_readback_incomplete_or_duplicate");
        return;
    }
    // Only actual callback bytes populate observed fields. Expected bytes and all
    // route/source/frame metadata were frozen before the four GPU copies.
    const String hex = String::hex_encode_buffer(p_bytes.ptr(), p_bytes.size());
    if (p_slot < 3) {
        static const char *keys[3] = { "atlas_hex", "instance_hex", "grading_hex" };
        observation.result[keys[p_slot]] = hex;
    } else {
        GaussianSplatting::IndirectDispatchLayout header = {};
        memcpy(&header, p_bytes.ptr(), sizeof(header));
        Dictionary gpu_count = observation.result["gpu_sort_count"];
        gpu_count["header_hex"] = hex;
        gpu_count["header_bytes"] = p_bytes.size();
        gpu_count["dispatch_x"] = header.dispatch_x;
        gpu_count["dispatch_y"] = header.dispatch_y;
        gpu_count["dispatch_z"] = header.dispatch_z;
        gpu_count["element_count"] = header.element_count;
        gpu_count["overflow_flag"] = header.overflow_flag;
        gpu_count["unclamped_total"] = header.unclamped_total;
        observation.result["gpu_sort_count"] = gpu_count;
    }
    observation.received_mask |= 1u << p_slot;
    if (observation.received_mask == 15u) {
        observation.result["readback_complete"] = true;
        observation.result["readback_completed_mask"] = observation.received_mask;
        observation.complete = true;
    }
}

Dictionary GaussianSplatRenderer::test_observe_hlod_publication(int64_t p_request_id) {
    Dictionary result;
    // Explicit TESTS-only snapshots of a bounded main-view fixture. Submit with
    // request_id=0, then poll that request while the normal frame loop advances.
    RenderingServer *server = RenderingServer::get_singleton();
    if (!server || !server->is_on_render_thread()) { result["error"] = "render_thread_required"; return result; }
    if (teardown_resources_started.load(std::memory_order_acquire)) { result["error"] = "renderer_teardown"; return result; }
    if (p_request_id != 0) {
        const Ref<HlodPublicationObservation> observation = test_hlod_observation;
        if (observation.is_null()) { result["error"] = "publication_readback_request_mismatch"; result["fatal"] = true; return result; }
        {
            MutexLock lock(observation->mutex);
            if (p_request_id < 0 || observation->request_id != uint64_t(p_request_id)) {
                result["error"] = "publication_readback_request_mismatch"; result["fatal"] = true; return result;
            }
            result["observation_request_id"] = p_request_id;
            result["pending"] = !observation->complete;
            if (!observation->complete) { return result; }
            result = observation->result.duplicate(true);
            result["pending"] = false;
            result["readback_poll_renderer_frame"] = int64_t(get_frame_state().frame_counter);
        }
        test_hlod_observation.unref();
        // A completed historical snapshot cannot certify a newly installed source
        // or device. Frame/content serials may advance normally during async work;
        // their frozen values still identify the copied frame, never the poll frame.
        const auto &state = get_resource_state();
        const Ref<GaussianData> source = get_scene_state().gaussian_data;
        RenderingDevice *device = get_device_state().rd;
        auto *director = GaussianSplatSceneDirector::get_singleton();
        if (!result.has("error") && (!device || !director || source.is_null() ||
                int64_t(device->get_device_instance_id()) != int64_t(result["device_instance_id"]) ||
                int64_t(source->get_instance_id()) != int64_t(result["source_data_id"]) ||
                int64_t(source->get_content_revision()) != int64_t(result["source_revision"]) ||
                state.hlod_snapshot.is_null() || !state.hlod_snapshot->is_current() ||
                int64_t(state.hlod_snapshot->get_instance_id()) != int64_t(result["input_source_generation"]) ||
                int64_t(director->get_instance_generation_for_renderer(this)) != int64_t(result["current_instance_generation"]) ||
                int64_t(state.instance_grading_defaults_generation.load(std::memory_order_relaxed)) != int64_t(result["current_grading_generation"]))) {
            result["error"] = "publication_readback_identity_changed";
        }
        return result;
    }
    if (test_hlod_observation.is_valid()) { result["error"] = "publication_readback_busy"; result["fatal"] = true; return result; }
    const uint64_t copy_renderer_frame = get_frame_state().frame_counter;
    result["copy_renderer_frame"] = int64_t(copy_renderer_frame);
    const auto &state = get_resource_state();
    const auto &bound = get_instance_pipeline_buffers();
    RenderingDevice *rd = get_device_state().rd;
    const Ref<GaussianData> source = get_scene_state().gaussian_data;
    if (!rd || rd != server->get_rendering_device() || source.is_null() || source->get_count() == 0 || source->get_count() > 64 ||
            bound.hlod_relative || bound.quantization_required || !bound.atlas_gaussian_buffer.is_valid() ||
            !rd->buffer_is_valid(bound.atlas_gaussian_buffer)) {
        result["error"] = "current_small_raw_canonical_fixture_required";
        return result;
    }
    GaussianData::SaveSnapshot snapshot;
    if (source->capture_save_snapshot(snapshot) != OK) { result["error"] = "source_snapshot_failed"; return result; }
    Vector<uint8_t> expected;
    const uint32_t count = snapshot.get_gaussians().size();
    if (count == 0 || count > 64) { result["error"] = "current_small_raw_canonical_fixture_required"; return result; }
    if (!gs_resize_or_fail(expected, uint64_t(count) * sizeof(PackedGaussian), "HLOD test expected raw bytes")) {
        result["error"] = "expected_payload_allocation_failed"; return result;
    }
    SHCompressionMetrics metrics;
    const uint32_t high_count = snapshot.get_sh_high_order_count();
    for (uint32_t i = 0; i < count; ++i) {
        PackedGaussian packed = {};
        pack_gaussian(snapshot.get_gaussians()[i], packed, metrics,
                high_count ? snapshot.get_sh_high_order().ptr() + uint64_t(i) * high_count : nullptr,
                snapshot.get_sh_first_order_count(), high_count);
        memcpy(expected.ptrw() + uint64_t(i) * sizeof(packed), &packed, sizeof(packed));
    }
    result["source_leaf_hex"] = String::hex_encode_buffer(expected.ptr(), expected.size());
    result["source_data_id"] = int64_t(source->get_instance_id());
    result["device_instance_id"] = int64_t(rd->get_device_instance_id());
    result["source_revision"] = int64_t(snapshot.get_content_revision());
    result["source_leaf_count"] = count;
    result["source_first_order_count"] = snapshot.get_sh_first_order_count();
    result["source_high_order_count"] = high_count;
    result["bound_atlas_count"] = bound.atlas_gaussian_count;
    result["input_source_generation"] = state.hlod_snapshot.is_valid() ? int64_t(state.hlod_snapshot->get_instance_id()) : int64_t(0);
    result["input_current"] = state.hlod_snapshot.is_valid() && state.hlod_snapshot->is_current();
    result["prepared_source_generation"] = state.hlod_prepared_snapshot.is_valid() ? int64_t(state.hlod_prepared_snapshot->get_instance_id()) : int64_t(0);
    result["prepared_current"] = is_hlod_prepared_contract_current();
    result["failure_mode"] = state.hlod_test_failure.load(std::memory_order_acquire);
    result["failure_hits"] = int64_t(state.hlod_test_failure_hits.load(std::memory_order_relaxed));
    LocalVector<InstanceDataGPU> instances;
    LocalVector<InstanceGradingGPU> grading;
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director || !bound.instance_buffer.is_valid() || !bound.instance_grading_buffer.is_valid()) {
        result["error"] = "current_instance_contract_missing"; return result;
    }
    if (instance_contract_shape == "hlod_canonical") {
        // The HLOD bundle's own rows (PR1b: row 0 the world, rows 1..N the node instances), packed as uploaded. Without node
        // instances this is the one world row build_instance_buffer_for_renderer gives.
        gs_hlod_bundle::Rows hlod_rows;
        LocalVector<uint64_t> hlod_keys;
        director->build_hlod_instance_rows_for_renderer(this, hlod_rows.instances, hlod_rows.grading, hlod_rows.asset_ids, hlod_keys);
        if (gs_hlod_bundle::rows_refusal(hlod_rows, state.hlod_prepared_snapshot.is_valid() ? uint64_t(state.hlod_prepared_snapshot->nodes.size()) : 0u,
                    state.hlod_instance_region) != nullptr) {
            result["error"] = "current_hlod_rows_unpackable"; return result;
        }
        gs_hlod_bundle::pack_instance_rows(hlod_rows, state.hlod_instance_region, instances);
        grading = hlod_rows.grading;
    } else {
        director->build_instance_buffer_for_renderer(this, instances, false);
        director->build_instance_grading_buffer_for_renderer(this, grading, false);
    }
    if (instance_contract_shape != "hlod_canonical") {
        // Ordinary resident publication adds its remap generation in lod[1].
        // This is not source payload and is intentionally different from the
        // direct HLOD identity shim; compare each route's complete actual ABI.
        if (!instance_asset_remap.valid || instances.size() != 1) {
            result["error"] = "current_resident_remap_missing"; return result;
        }
        instances[0].ids[0] = 0u;
        instances[0].lod[1] = instance_asset_remap.generation == 0 ? 1u : uint32_t(instance_asset_remap.generation & uint64_t(UINT32_MAX));
    }
    result["expected_instance_hex"] = String::hex_encode_buffer(reinterpret_cast<const uint8_t *>(instances.ptr()), instances.size() * sizeof(InstanceDataGPU));
    result["expected_grading_hex"] = String::hex_encode_buffer(reinterpret_cast<const uint8_t *>(grading.ptr()), grading.size() * sizeof(InstanceGradingGPU));
    result["current_instance_generation"] = int64_t(director->get_instance_generation_for_renderer(this));
    result["current_grading_generation"] = int64_t(state.instance_grading_defaults_generation.load(std::memory_order_relaxed));
    result["relative_capacity"] = MIN<uint32_t>(count, MAX(1, get_performance_settings().max_splats));
    // The frame has already restored the canonical bundle. Use the RID captured
    // on the actual sort route, not the currently bound canonical count buffer.
    const RID count_buffer = state.hlod_test_sort_count_buffer;
    const RID expected_count_buffer = state.hlod_frame_active
            ? state.hlod_relative.buffers.instance_count_buffer : bound.instance_count_buffer;
    if (state.hlod_test_sort_frame != get_frame_state().frame_counter ||
            state.hlod_test_sort_source != uint64_t(source->get_instance_id()) ||
            state.hlod_snapshot.is_null() ||
            state.hlod_test_sort_input_generation != uint64_t(state.hlod_snapshot->get_instance_id()) ||
            state.hlod_test_sort_device != rd->get_device_instance_id() ||
            state.hlod_test_sort_relative != state.hlod_frame_active ||
            (state.hlod_frame_active && state.hlod_test_sort_content_generation != state.hlod_relative.buffers.hlod_content_generation) ||
            !count_buffer.is_valid() || count_buffer != expected_count_buffer ||
            get_resource_owner(count_buffer, nullptr) != rd || !rd->buffer_is_valid(count_buffer) ||
            subsystem_state.sorting_pipeline.is_null()) {
        result["error"] = "current_resident_sort_count_provenance_required"; return result;
    }
    Dictionary gpu_count;
    gpu_count["count_buffer_rid"] = int64_t(count_buffer.get_id());
    gpu_count["renderer_frame"] = int64_t(state.hlod_test_sort_frame);
    gpu_count["pass_frame"] = int64_t(state.hlod_test_sort_pass_frame);
    gpu_count["source_data_id"] = int64_t(state.hlod_test_sort_source);
    gpu_count["input_source_generation"] = int64_t(state.hlod_test_sort_input_generation);
    gpu_count["device_instance_id"] = int64_t(state.hlod_test_sort_device);
    gpu_count["content_generation"] = int64_t(state.hlod_test_sort_content_generation);
    gpu_count["relative"] = state.hlod_test_sort_relative;
    result["gpu_sort_count"] = gpu_count;
    Dictionary async_count;
    async_count["valid"] = subsystem_state.sorting_pipeline->_test_get_last_instance_visible_splat_count_valid();
    async_count["sample_pass_frame"] = int64_t(subsystem_state.sorting_pipeline->_test_get_last_instance_visible_splat_count_frame());
    async_count["element_count"] = subsystem_state.sorting_pipeline->get_last_instance_visible_splat_count();
    result["async_sort_count"] = async_count;
    result["stats"] = get_render_stats().duplicate(true);
    result["rendered_content"] = has_rendered_content();
    const RID buffers[4] = { bound.atlas_gaussian_buffer, bound.instance_buffer, bound.instance_grading_buffer, count_buffer };
    // Preserve whole-buffer coverage (size=0), not a matching prefix of an
    // oversized buffer. Every callback must have the complete expected ABI size.
    for (const RID &buffer : buffers) {
        if (get_resource_owner(buffer, nullptr) != rd || !rd->buffer_is_valid(buffer)) {
            result["error"] = "publication_readback_buffer_owner_mismatch"; return result;
        }
    }
    if (test_hlod_observation_serial >= uint64_t(INT64_MAX)) { result["error"] = "publication_request_id_exhausted"; result["fatal"] = true; return result; }
    const uint64_t request_id = ++test_hlod_observation_serial;
    result["observation_request_id"] = int64_t(request_id);
    Ref<HlodPublicationObservation> observation;
    observation.instantiate();
    observation->request_id = request_id;
    observation->device_id = rd->get_device_instance_id();
    observation->expected_bytes[0] = expected.size();
    observation->expected_bytes[1] = uint64_t(instances.size()) * sizeof(InstanceDataGPU);
    observation->expected_bytes[2] = uint64_t(grading.size()) * sizeof(InstanceGradingGPU);
    observation->expected_bytes[3] = sizeof(GaussianSplatting::IndirectDispatchLayout);
    observation->result = result.duplicate(true);
    test_hlod_observation = observation;
    // Do not hold the box mutex across RD calls: staging pressure can deliver
    // an earlier callback while a new transfer is being queued.
    for (int slot = 0; slot < 4; ++slot) {
        const Callable callback = HlodPublicationObservation::make_callback(observation, slot);
        const Error error = rd->buffer_get_data_async(buffers[slot], callback, 0, 0);
        if (error != OK) {
            observation->fail("publication_readback_submission_failed");
            break;
        }
    }
    if (get_frame_state().frame_counter != copy_renderer_frame) {
        observation->fail("renderer_frame_changed_during_publication_copy");
    }
    Dictionary pending;
    pending["pending"] = true;
    pending["observation_request_id"] = int64_t(request_id);
    return pending;
}

Dictionary GaussianSplatRenderer::test_hlod_foreign_device_publication() {
    Dictionary result;
    RenderingServer *server = RenderingServer::get_singleton();
    if (!server || !server->is_on_render_thread() || !is_hlod_prepared_contract_current()) {
        result["error"] = "current_prepared_render_thread_fixture_required"; return result;
    }
    RenderingDevice *original = get_device_state().rd;
    RenderingDevice *other = server ? server->create_local_rendering_device() : nullptr;
    if (!other) { result["error"] = "second_real_device_unavailable"; return result; }
    result["original_device"] = int64_t(original->get_device_instance_id());
    result["foreign_device"] = int64_t(other->get_device_instance_id());
    // A real different current RD must never bind buffers owned by the first.
    // No foreign dispatch is submitted. Restore the device before its teardown;
    // the normal next frame must rebuild/recover on the actual main-view device.
    get_device_state().rd = other;
    result["publication_succeeded"] = publish_hlod_bundle(false);
    result["prepared_retained"] = get_resource_state().hlod_prepared_snapshot.is_valid();
    get_device_state().rd = original;
    memdelete(other);
    return result;
}

Ref<Resource> GaussianSplatRenderer::test_bake_hlod_small_leaf_fixture(const Ref<GaussianData> &p_data) {
    ERR_FAIL_COND_V(p_data.is_null() || p_data->get_count() < 3 || p_data->get_count() > 64, Ref<Resource>());
    GaussianData::SaveSnapshot snapshot;
    ERR_FAIL_COND_V(p_data->capture_save_snapshot(snapshot) != OK, Ref<Resource>());
    gs_hlod::BakeInput input;
    input.gaussians = snapshot.get_gaussians().ptr();
    input.count = snapshot.get_gaussians().size();
    input.sh_first_order_count = snapshot.get_sh_first_order_count();
    input.sh_high_order_count = snapshot.get_sh_high_order_count();
    input.sh_high_order = snapshot.get_sh_high_order().is_empty() ? nullptr : snapshot.get_sh_high_order().ptr();
    gs_hlod::BakeParams params;
    params.leaf_max_splats = 2; // Existing baker's fixture parameter; quality algorithm/thresholds unchanged.
    gs_hlod::BakeResult baked;
    String reason;
    ERR_FAIL_COND_V_MSG(!gs_hlod::bake_world(input, params, baked, &reason), Ref<Resource>(), reason);
    Vector<StaticChunk> chunks;
    ERR_FAIL_COND_V(!GaussianSplatWorld::build_hlod_leaf_chunks(baked.tree, chunks), Ref<Resource>());
    Ref<GaussianData> leaves;
    leaves.instantiate();
    leaves->set_gaussian_payload(baked.leaf_gaussians, baked.leaf_sh_high_order,
            input.sh_first_order_count, input.sh_high_order_count, snapshot.get_2d_mode(), snapshot.get_antialiased());
    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(leaves);
    world->set_static_chunks(chunks);
    world->set_hlod_tree(std::move(baked.tree));
    return world;
}
#endif // TESTS_ENABLED

// ---------------------------------------------------------------------------------------------------------------------
// Package F1: editable regions on the streamed HLOD route (internal API; see gaussian_splat_renderer.h).
// ---------------------------------------------------------------------------------------------------------------------

Ref<GaussianSplatHlodRuntimeData> GaussianSplatRenderer::get_hlod_served_snapshot() const {
    const auto &state = get_resource_state();
    if (state.hlod_streaming && state.hlod_streamer.is_valid() && state.hlod_streamer->is_active()) {
        return state.hlod_streamer->get_data();
    }
    return Ref<GaussianSplatHlodRuntimeData>();
}

bool GaussianSplatRenderer::stage_hlod_edit(const Ref<GaussianSplatHlodRuntimeData> &p_edited, String &r_reason) {
    auto &state = get_resource_state();
    if (!state.hlod_streaming || state.hlod_streamer.is_null() || !state.hlod_streamer->is_active()) {
        r_reason = "hlod_edit_requires_streamed_route";
        state.hlod_edit_event = r_reason;
        return false;
    }
    switch (state.hlod_streamer->stage_edit(p_edited)) {
        case HlodNodeStreamer::EDIT_STAGED: r_reason = "hlod_edit_staged"; break;
        case HlodNodeStreamer::EDIT_REJECTED_BUSY: r_reason = "hlod_edit_busy"; break;
        case HlodNodeStreamer::EDIT_REJECTED_DATA: r_reason = "hlod_edit_snapshot_mismatch"; break;
        case HlodNodeStreamer::EDIT_REJECTED_NO_SPACE: r_reason = "hlod_edit_no_space"; break;
        case HlodNodeStreamer::EDIT_REJECTED_INFEASIBLE: r_reason = "hlod_edit_replacement_cut_infeasible"; break;
        case HlodNodeStreamer::EDIT_REJECTED_NO_VIEW: r_reason = "hlod_edit_no_registered_view"; break;
        case HlodNodeStreamer::EDIT_REJECTED_LINEAGE: r_reason = "hlod_edit_lineage"; break;
    }
    state.hlod_edit_event = r_reason;
    return r_reason == "hlod_edit_staged";
}

bool GaussianSplatRenderer::hlod_edit_committed_here() const {
    const auto &state = get_resource_state();
    return state.hlod_committed_edit.is_valid() && state.hlod_edit_owner_key == state.hlod_owner_key &&
            state.hlod_edit_world_key == state.hlod_world_key;
}

GaussianSplatRenderer::HlodRouteDecision GaussianSplatRenderer::decide_hlod_route(bool p_committed_edit, bool p_served_has_edits, bool p_paged,
        bool p_should_stream, const char *p_streaming_unsupported, String &r_reason) {
    if (p_committed_edit && !p_served_has_edits) {
        r_reason = "hlod_edit_rebase_refused";
        return HlodRouteDecision::REFUSE;
    }
    if (p_served_has_edits && p_streaming_unsupported != nullptr) {
        r_reason = String("hlod_edit_requires_streamed_route:") + p_streaming_unsupported;
        return HlodRouteDecision::REFUSE;
    }
    const bool streaming = (p_paged || p_served_has_edits || p_should_stream) && p_streaming_unsupported == nullptr;
    return streaming ? HlodRouteDecision::BUILD_STREAMED : HlodRouteDecision::BUILD_WHOLE_TREE;
}

GaussianSplatRenderer::HlodEditFrame GaussianSplatRenderer::decide_hlod_edit_frame(bool p_committed_here, bool p_blanks_frame, int p_hlod_result,
        bool p_has_other_instances) {
    // A result <= 0 means the HLOD route did not publish this frame (idle failed, sort buffers invalid, not taken...): the canonical
    // copy would then be the UNEDITED world even where a streamer is alive.
    if (!p_committed_here || (!p_blanks_frame && p_hlod_result > 0)) {
        return HlodEditFrame::PUBLISH;
    }
    return p_has_other_instances ? HlodEditFrame::PUBLISH_WITHOUT_EDITED_WORLD : HlodEditFrame::SKIP_FRAME;
}

String GaussianSplatRenderer::compose_hlod_edit_blank_reason(const String &p_cause, bool p_cut_enabled) {
    if (p_cause.begins_with("hlod_edit_")) {
        return p_cause;
    }
    String cause = p_cause;
    if (cause.is_empty() || cause == "active") {
        cause = p_cut_enabled ? String() : String("hlod_runtime_cut_disabled");
    }
    return cause.is_empty() ? String("hlod_edit_route_unavailable") : String("hlod_edit_route_unavailable:") + cause;
}

bool GaussianSplatRenderer::hlod_committed_edit_applies_to(const Ref<GaussianSplatHlodRuntimeData> &p_capture) const {
    const auto &state = get_resource_state();
    if (state.hlod_committed_edit.is_null() || p_capture.is_null() || !hlod_edit_committed_here()) {
        return false;
    }
    // The same world replaced in place (reimport): its content revision moved. A paged <-> resident switch (the world was
    // materialized by a cut toggle) is not a content change and keeps the edit.
    return !(state.hlod_committed_edit->is_paged() == p_capture->is_paged() && state.hlod_committed_edit->source_revision != p_capture->source_revision);
}

uint32_t GaussianSplatRenderer::hlod_dispatch_extent_for(uint32_t p_current, const GaussianSplatHlodRuntimeData &p_data) {
    uint32_t extent = p_current;
    for (int n = 0; n < p_data.nodes.size(); ++n) {
        extent = MAX(extent, p_data.nodes[n].topology.payload_count);
    }
    return extent;
}

Ref<GaussianSplatHlodRuntimeData> GaussianSplatRenderer::adopt_hlod_served_snapshot() {
    auto &state = get_resource_state();
    Ref<GaussianSplatHlodRuntimeData> served = state.hlod_streamer->get_data();
    if (served->has_edits() && state.hlod_committed_edit != served) {
        state.hlod_committed_edit = served;
        state.hlod_edit_owner_key = state.hlod_prepared_owner_key;
        state.hlod_edit_world_key = state.hlod_world_key;
        // An edited leaf may be larger than every captured one (up to 16,384 splats). The rebuild path raises the extent
        // for an edited snapshot; the live commit must do the same before the new cut is published.
        state.hlod_relative.buffers.max_chunk_splats = hlod_dispatch_extent_for(state.hlod_relative.buffers.max_chunk_splats, *served.ptr());
    }
    return served;
}

Ref<GaussianSplatHlodRuntimeData> GaussianSplatRenderer::hlod_stream_commit_boundary() {
    auto &state = get_resource_state();
    HlodNodeStreamer::Buffers frame_buffers;
    frame_buffers.atlas = state.hlod_relative.buffers.atlas_gaussian_buffer;
    frame_buffers.chunk_meta = state.hlod_relative.buffers.chunk_meta_buffer;
    frame_buffers.quantization = state.hlod_relative.buffers.quantization_buffer;
    // The publication boundary of a staged edit is inside begin_frame(): commit first.
    state.hlod_streamer->begin_frame(get_device_state().rd, &frame_buffers);
    if (!state.hlod_streamer->is_active()) {
        return Ref<GaussianSplatHlodRuntimeData>(); // A failed commit part-way: the caller releases the bundle.
    }
    return adopt_hlod_served_snapshot();
}

void GaussianSplatRenderer::register_hlod_known_view(const HlodViewIdentity &p_identity, const gs_hlod_runtime::CutRequest &p_request) {
    auto &state = get_resource_state();
    if (state.hlod_streamer.is_null()) { return; }
    sync_hlod_known_view(*state.hlod_streamer.ptr(),
            [this](uint64_t p_key) { return frame_context_manager.is_hlod_view_live(p_key); }, p_identity, p_request);
}

void GaussianSplatRenderer::sync_hlod_known_view(HlodNodeStreamer &r_streamer, const std::function<bool(uint64_t)> &p_view_live,
        const HlodViewIdentity &p_identity, const gs_hlod_runtime::CutRequest &p_request) {
    const uint64_t key = RenderFrameContextManager::hlod_view_key(p_identity);
    for (uint32_t i = r_streamer.registered_view_count(); i-- > 0;) {
        const uint64_t other = r_streamer.registered_view_key(i);
        if (other != key && !p_view_live(other)) { r_streamer.unregister_view(other); }
    }
    r_streamer.register_view(key, p_request);
}

bool GaussianSplatRenderer::hlod_edit_served() const {
    const auto &state = get_resource_state();
    if (state.hlod_committed_edit.is_null() || !state.hlod_streaming || state.hlod_streamer.is_null() || !state.hlod_streamer->is_active()) {
        return false;
    }
    const Ref<GaussianSplatHlodRuntimeData> &data = state.hlod_streamer->get_data();
    return data.is_valid() && data->edit_revision == state.hlod_committed_edit->edit_revision &&
            data->source_world == state.hlod_committed_edit->source_world;
}

void GaussianSplatRenderer::release_hlod_committed_edit() {
    auto &state = get_resource_state();
    state.hlod_committed_edit.unref();
    state.hlod_edit_owner_key = 0;
    state.hlod_edit_world_key = 0;
}

void GaussianSplatRenderer::reconcile_hlod_committed_edit() {
    auto &state = get_resource_state();
    if (state.hlod_committed_edit.is_valid() &&
            (state.hlod_edit_owner_key != state.hlod_owner_key || state.hlod_edit_world_key != state.hlod_world_key)) {
        release_hlod_committed_edit(); // A different node or a different world: the edit does not apply.
    }
}

void GaussianSplatRenderer::apply_hlod_edit_backend_override(bool p_committed_here, FrameBackendPlan &r_plan) {
    if (p_committed_here) {
        r_plan.prefer_resident_backend = true;
        r_plan.should_attempt_streaming_bootstrap = false; // Do not even build the legacy streaming system for it (F-4).
    }
}

bool GaussianSplatRenderer::hlod_edit_blanks_frame(String &r_reason) const {
    // True when this world has committed edits that the current route does not serve: every route that is not the streamed
    // HLOD route serving the committed snapshot would redisplay the OLD content (or an unedited rebuild would).
    if (!hlod_edit_committed_here() || hlod_edit_served()) {
        return false;
    }
    const auto &state = get_resource_state();
    // The cause is the route's own reason; an hlod_edit_* reason (this one, from the last frame) is not a cause, so it is
    // never wrapped again. A disabled cut has no snapshot and so no reason of its own: name it.
    r_reason = compose_hlod_edit_blank_reason(state.hlod_reason, gs::settings::get_hlod_runtime_cut_enabled(ProjectSettings::get_singleton()));
    return true;
}

#ifdef TESTS_ENABLED
// Builds the box-deletion edit of one probe phase on the served snapshot and stages it. Leaves already edited are skipped,
// so a second box edits other leaves. Returns the result string (a stats key value).
static String hlod_edit_probe_stage(GaussianSplatRenderer *p_renderer, const String &p_box, bool p_fail_commit) {
    // GS_HLOD_EDIT_MODE=recolor paints every splat of the touched leaves red instead of deleting the splats inside the box:
    // an edit whose effect is large and unmistakable at any distance (frame classification in the probe).
    const bool recolor = OS::get_singleton()->get_environment("GS_HLOD_EDIT_MODE") == "recolor";
    auto &state = p_renderer->get_resource_state();
    const Vector<String> parts = p_box.split(",");
    if (parts.size() != 6) {
        return "bad_box";
    }
    Vector3 lo(parts[0].to_float(), parts[1].to_float(), parts[2].to_float());
    Vector3 hi(parts[3].to_float(), parts[4].to_float(), parts[5].to_float());
    const Ref<GaussianSplatHlodRuntimeData> served = state.hlod_streamer->get_data();
    LocalVector<gs_hlod_runtime::LeafEdit> edits;
    uint64_t deleted = 0;
    for (uint32_t n = 0; n < uint32_t(served->nodes.size()) && edits.size() < gs_hlod_runtime::kMaxEditedLeaves; ++n) {
        const auto &node = served->nodes[n];
        if (node.topology.child_count != 0 || served->find_edited(n) != nullptr) { continue; }
        // Sphere / box overlap on the leaf's bounds.
        const Vector3 c = node.bounds_center;
        const Vector3 nearest = c.clamp(lo, hi);
        if (c.distance_to(nearest) > node.topology.radius) { continue; }
        gs_hlod_runtime::LeafEdit edit;
        edit.node = n;
        Vector<Gaussian> all;
        Vector<Vector3> all_sh;
        if (served->is_paged()) {
            String why;
            if (served->paged_source->read_node_payload(n, all, all_sh, why) != OK) { continue; }
        } else {
            for (uint32_t i = 0; i < node.topology.payload_count; ++i) {
                all.push_back(served->gaussian(node.topology.payload_first + i));
                const Vector3 *sh = served->sh(node.topology.payload_first + i);
                for (uint32_t k = 0; sh && k < served->high_order_count; ++k) { all_sh.push_back(sh[k]); }
            }
        }
        uint64_t inside = 0;
        for (int64_t i = 0; recolor && i < all.size(); ++i) {
            const Vector3 q = all[i].position;
            inside += (q.x >= lo.x && q.x <= hi.x && q.y >= lo.y && q.y <= hi.y && q.z >= lo.z && q.z <= hi.z) ? 1u : 0u;
        }
        if (recolor && inside == 0) { continue; } // Only leaves that actually hold splats of the box are repainted.
        for (int64_t i = 0; recolor && i < all.size(); ++i) {
            Gaussian g = all[i];
            g.sh_dc = Color(1.0f, 0.0f, 0.0f, 1.0f);
            edit.gaussians.push_back(g);
            for (uint32_t k = 0; k < served->high_order_count; ++k) { edit.sh.push_back(Vector3()); }
        }
        for (int64_t i = 0; !recolor && i < all.size(); ++i) {
            const Vector3 p = all[i].position;
            if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z) { continue; }
            edit.gaussians.push_back(all[i]);
            for (uint32_t k = 0; k < served->high_order_count; ++k) { edit.sh.push_back(all_sh[i * served->high_order_count + k]); }
        }
        if (edit.gaussians.is_empty() && !all.is_empty()) {
            // A leaf may not become empty (ADR 14.5.1): keep its first splat.
            edit.gaussians.push_back(all[0]);
            for (uint32_t k = 0; k < served->high_order_count; ++k) { edit.sh.push_back(all_sh[k]); }
        }
        if (!recolor && edit.gaussians.size() == all.size()) { continue; } // Nothing deleted here.
        deleted += uint64_t(all.size() - edit.gaussians.size());
        edits.push_back(edit);
    }
    if (edits.is_empty()) {
        return "no_leaf_touched";
    }
    Ref<GaussianSplatHlodRuntimeData> edited;
    String reason;
    const gs_hlod_runtime::EditStatus status = gs_hlod_runtime::make_edited_snapshot(served, served->edit_revision, edits.ptr(), edits.size(), edited, reason);
    if (status != gs_hlod_runtime::EditStatus::OK) {
        return "factory:" + reason;
    }
    state.hlod_streamer->test_fail_commit_metadata = p_fail_commit; // Forces COMMIT_UNUSABLE for this edit only.
    p_renderer->stage_hlod_edit(edited, reason);
    return vformat("%s leaves=%d deleted=%d", reason, int64_t(edits.size()), int64_t(deleted));
}

void GaussianSplatRenderer::hlod_edit_probe_step() {
    auto &state = get_resource_state();
    if (!state.hlod_streaming || state.hlod_streamer.is_null() || !state.hlod_streamer->is_active() ||
            state.hlod_streamer->is_edit_staged()) {
        return;
    }
    // Phase 0: GS_HLOD_EDIT_BOX once the view has been ready for a while. Phase 1: GS_HLOD_EDIT_BOX2 (optionally with a
    // forced COMMIT_UNUSABLE, GS_HLOD_EDIT_FAIL2=1) once the first edit is committed and the view has run 200 ready frames.
    if (state.hlod_edit_probe_phase == 0 && state.hlod_stream_ready_frames >= 30) {
        const String box = OS::get_singleton()->get_environment("GS_HLOD_EDIT_BOX");
        state.hlod_edit_probe_phase = 1;
        if (!box.is_empty()) {
            state.hlod_edit_probe_result = hlod_edit_probe_stage(this, box, false);
        }
    } else if (state.hlod_edit_probe_phase == 1 && state.hlod_stream_ready_frames >= 200 &&
            state.hlod_streamer->get_data()->edit_revision >= 1) {
        const String box2 = OS::get_singleton()->get_environment("GS_HLOD_EDIT_BOX2");
        state.hlod_edit_probe_phase = 2;
        if (!box2.is_empty()) {
            state.hlod_edit_probe_result = "phase2:" + hlod_edit_probe_stage(this, box2, OS::get_singleton()->get_environment("GS_HLOD_EDIT_FAIL2") == "1");
        }
    }
}
#endif
