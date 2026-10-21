#include "resident_instance_contract_publisher.h"

#include "../core/gs_vector_alloc.h"
#include "gaussian_gpu_layout.h"
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
#include "servers/rendering_server.h"
#include "core/config/engine.h"
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
    const int hlod_result = p_renderer->try_publish_hlod_resident_contract(hlod_reason);
    if (hlod_result != 0) {
        if (r_reason) { *r_reason = hlod_reason; }
        return hlod_result > 0;
    }

	// Resident atlas VRAM budget (#321 follow-up). A default GaussianSplatNode3D is
	// resident-only by contract, so an over-large atlas cannot fall back to streaming and
	// would OOM / device-lost on a low-end GPU. Clamp the packed atlas to the device VRAM
	// budget (or an explicit project override) and pack an importance-ordered subset below
	// instead. Unknown/UMA budgets and capable GPUs resolve to the 2 GB staging ceiling, so
	// the effective cap == the staging ceiling and nothing below changes.
	const uint64_t device_budget_bytes = rd->get_device_memory_budget();
	const uint32_t resident_atlas_override_mb = gs::settings::get_uint(ProjectSettings::get_singleton(),
			"rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb", 0u);
	const uint64_t effective_atlas_cap_bytes =
			ResidentAtlasBudget::compute_effective_atlas_cap_bytes(device_budget_bytes, resident_atlas_override_mb);

	const Ref<GaussianData> primary_data = p_renderer->get_scene_state().gaussian_data;
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

uint64_t hlod_bundle_bytes(const GaussianSplatHlodRuntimeData &p_data, bool p_relative, bool p_quantized, uint32_t p_capacity) {
    const uint64_t nodes = p_data.nodes.size();
    const uint64_t records = uint64_t(p_data.leaf_count) + (p_relative ? p_data.parent_count : 0);
    return records * (p_quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian)) +
            nodes * (sizeof(ChunkMetaGPU) + sizeof(AssetChunkIndexGPU) + sizeof(VisibleChunkRefGPU) +
                    (p_quantized ? sizeof(ChunkQuantizationGPU) : 0)) +
            sizeof(AssetMetaGPU) + sizeof(InstanceDataGPU) + sizeof(InstanceGradingGPU) +
            uint64_t(p_capacity) * (sizeof(SplatRefGPU) + (p_relative ? sizeof(uint32_t) : 0)) +
            6 * sizeof(uint32_t) + 2 * sizeof(GaussianSplatting::IndirectDispatchLayout);
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
        uint32_t p_capacity, uint32_t p_position_bits, uint32_t p_scale_bits, const InstanceDataGPU &p_instance, const InstanceGradingGPU &p_grading, HlodBundle &r_bundle) {
    auto &b = r_bundle.buffers;
    const uint32_t node_count = p_data.nodes.size();
    const uint32_t records = p_data.leaf_count + (p_relative ? p_data.parent_count : 0);
    Vector<PackedGaussian> raw;
    Vector<PackedGaussianQuantized> quantized;
    Vector<ChunkMetaGPU> metadata;
    Vector<AssetChunkIndexGPU> indices;
    Vector<ChunkQuantizationGPU> quantization;
    if (!gs_resize_or_fail(metadata, node_count, "HLOD candidate metadata") ||
            !gs_resize_or_fail(indices, node_count, "HLOD candidate indices") ||
            !gs_resize_or_fail(r_bundle.draw_entries, node_count, "HLOD candidate draw list") ||
            (p_quantized ? (!gs_resize_or_fail(quantized, records, "HLOD candidate packed quantized") ||
                                   !gs_resize_or_fail(quantization, node_count, "HLOD candidate quantization")) :
                           !gs_resize_or_fail(raw, records, "HLOD candidate packed raw"))) {
        return false;
    }
    metadata.fill(ChunkMetaGPU{});
    if (p_quantized) { quantization.fill(ChunkQuantizationGPU{}); }
    uint32_t chunk_count = 0;
    SHCompressionMetrics metrics;
    for (uint32_t n = 0; n < node_count; ++n) {
        const auto &node = p_data.nodes[n];
        if (!p_relative && !node.topology.is_leaf()) { continue; }
        const uint32_t first = uint32_t(node.topology.payload_first);
        const uint32_t count = node.topology.payload_count;
        indices.write[chunk_count++].chunk_id = n;
        ChunkMetaGPU &meta = metadata.write[n];
        meta.atlas_base = first;
        meta.splat_count = count;
        meta.quant_base = n;
        meta.quant_count = p_quantized ? 1 : 0;
        meta.sh_limit = p_data.sh_degree;
        meta.flags = gs_pack_asset_gpu_flags(p_data.mode_2d, GaussianDCEncoding(node.dc_encoding));
        for (int a = 0; a < 3; ++a) { meta.bounds_center_local[a] = node.bounds_center[a]; }
        meta.bounds_radius_local = node.topology.radius;
        meta.node_center_x = p_relative ? node.payload_center.x : 0.0f;
        meta.node_center_y = p_relative ? node.payload_center.y : 0.0f;
        meta.node_center_z = p_relative ? node.payload_center.z : 0.0f;
        b.max_chunk_splats = MAX(b.max_chunk_splats, count);
        auto local_gaussian = [&](uint32_t p_index) {
            Gaussian g = p_data.gaussian(p_index);
            if (p_relative && p_index < p_data.leaf_count) {
                for (int a = 0; a < 3; ++a) {
                    g.position[a] = float(double(g.position[a]) - node.exact_cell_center[a]);
                }
            }
            return g;
        };
        ChunkQuantizationInfo quant;
        if (p_quantized) {
            // The existing codec accepts LocalVector only. Build its same min/range inputs
            // directly from this immutable span; avoid an infallible temporary allocation.
            quant.position_bits = p_position_bits;
            quant.scale_bits = p_scale_bits;
            quant.scales_quantized = true;
            const Gaussian first_g = local_gaussian(first);
            quant.position_min = quant.position_max = first_g.position;
            quant.scale_min = quant.scale_max = first_g.scale;
            for (uint32_t i = 1; i < count; ++i) {
                const Gaussian g = local_gaussian(first + i);
                quant.position_min = quant.position_min.min(g.position);
                quant.position_max = quant.position_max.max(g.position);
                quant.scale_min = quant.scale_min.min(g.scale);
                quant.scale_max = quant.scale_max.max(g.scale);
            }
            quant.position_range = (quant.position_max - quant.position_min).max(Vector3(1e-6f, 1e-6f, 1e-6f));
            quant.scale_range = (quant.scale_max - quant.scale_min).max(Vector3(1e-6f, 1e-6f, 1e-6f));
            ChunkQuantizationGPU &gpu = quantization.write[n];
            for (int a = 0; a < 3; ++a) {
                gpu.position_min[a] = quant.position_min[a];
                gpu.position_range[a] = quant.position_range[a];
                gpu.scale_min[a] = quant.scale_min[a];
                gpu.scale_range[a] = quant.scale_range[a];
            }
            gpu.position_bits = quant.position_bits;
            gpu.scale_bits = quant.scale_bits;
            gpu.start_index = first;
            gpu.count = count;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t index = first + i;
            const Gaussian g = local_gaussian(index);
            if (p_quantized) {
                pack_gaussian_quantized(g, quant, uint16_t(n), quantized.write[index], metrics, p_data.sh(index),
                        p_data.leaves.get_sh_first_order_count(), p_data.high_order_count);
            } else {
                pack_gaussian(g, raw.write[index], metrics, p_data.sh(index),
                        p_data.leaves.get_sh_first_order_count(), p_data.high_order_count);
            }
        }
    }
    AssetMetaGPU asset = {};
    asset.lod_count = 1;
    asset.sh_degree = p_data.sh_degree;
    asset.flags = metadata[indices[0].chunk_id].flags;
    asset.chunk_index_count = chunk_count;
    asset.quant_chunk_count = p_quantized ? node_count : 0;
    asset.lod_ranges[0].count = chunk_count;
    for (int a = 0; a < 3; ++a) { asset.bounds_center_local[a] = p_data.nodes[0].bounds_center[a]; }
    asset.bounds_radius_local = p_data.nodes[0].topology.radius;
    auto create = [&](RID &r_rid, uint64_t p_bytes, const void *p_initial = nullptr, bool p_indirect = false) {
        return hlod_create_buffer(p_renderer, p_rd, r_bundle, r_rid, p_bytes, p_initial, p_indirect);
    };
    if (!create(b.atlas_gaussian_buffer, uint64_t(records) * (p_quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian)),
                p_quantized ? static_cast<const void *>(quantized.ptr()) : raw.ptr()) ||
            !create(b.asset_meta_buffer, sizeof(asset), &asset) ||
            !create(b.asset_chunk_index_buffer, uint64_t(node_count) * sizeof(AssetChunkIndexGPU), indices.ptr()) ||
            !create(b.chunk_meta_buffer, uint64_t(node_count) * sizeof(ChunkMetaGPU), metadata.ptr()) ||
            (p_quantized && !create(b.quantization_buffer, uint64_t(node_count) * sizeof(ChunkQuantizationGPU), quantization.ptr())) ||
            !create(b.instance_buffer, sizeof(p_instance), &p_instance) ||
            !create(b.instance_grading_buffer, sizeof(p_grading), &p_grading) ||
            !create(b.visible_chunk_buffer, uint64_t(node_count) * sizeof(VisibleChunkRefGPU)) ||
            !create(b.splat_ref_buffer, uint64_t(p_capacity) * sizeof(SplatRefGPU)) ||
            (p_relative && !create(b.hlod_draw_entry_buffer, uint64_t(p_capacity) * sizeof(uint32_t))) ||
            !create(b.counter_buffer, 3 * sizeof(uint32_t)) ||
            !create(b.chunk_dispatch_buffer, 3 * sizeof(uint32_t), nullptr, true) ||
            !create(b.indirect_count_buffer, sizeof(GaussianSplatting::IndirectDispatchLayout), nullptr, true) ||
            !create(b.instance_count_buffer, sizeof(GaussianSplatting::IndirectDispatchLayout))) {
        return false;
    }
    b.quantization_required = p_quantized;
    b.atlas_gaussian_count = records;
    b.instance_count = 1;
    b.dispatch_chunk_count = chunk_count;
    b.max_visible_chunks = node_count;
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
    return r_bundle.bytes == hlod_bundle_bytes(p_data, p_relative, p_quantized, p_capacity);
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
    if (get_instance_pipeline_buffers().atlas_gaussian_buffer == state.hlod_canonical.buffers.atlas_gaussian_buffer ||
            get_instance_pipeline_buffers().atlas_gaussian_buffer == state.hlod_relative.buffers.atlas_gaussian_buffer) {
        clear_instance_pipeline_buffers();
    }
    release_hlod_bundle(state.hlod_canonical);
    release_hlod_bundle(state.hlod_relative);
    state.hlod_prepared_snapshot.unref();
    state.hlod_active = false;
    state.hlod_frame_active = false;
    state.hlod_selected_nodes = state.hlod_selected_parents = state.hlod_visited_nodes = 0;
    state.hlod_selected_splats = 0;
    state.hlod_instance_generation = state.hlod_grading_generation = state.hlod_prepared_owner_key = 0;
    frame_context_manager.clear_hlod_histories();
}

bool GaussianSplatRenderer::is_hlod_prepared_contract_current() const {
    const auto &state = get_resource_state();
    const Ref<GaussianSplatHlodRuntimeData> input = state.hlod_prepared_snapshot;
    RenderingDevice *rd = get_device_state().rd;
    auto *director = GaussianSplatSceneDirector::get_singleton();
    if (input.is_null() || input != state.hlod_snapshot || !input->is_current() ||
            input->source_data != get_scene_state().gaussian_data || !rd || !director ||
            !gs::settings::get_hlod_runtime_cut_enabled(ProjectSettings::get_singleton()) ||
            !director->has_exclusive_hlod_world_submission(this, state.hlod_owner_key) ||
            state.hlod_prepared_owner_key != state.hlod_owner_key || is_shadow_instance_filter_enabled() ||
            state.hlod_instance_generation != director->get_instance_generation_for_renderer(this) ||
            state.hlod_grading_generation != state.instance_grading_defaults_generation.load(std::memory_order_relaxed) ||
            subsystem_state.sorting_pipeline.is_null() || subsystem_state.sorting_pipeline->get_sort_resource_device() != rd) {
        return false;
    }
    const bool quantized = g_quantization_config.per_chunk_quantization;
    if (quantized && (!input->quantized_eligible ||
            state.hlod_position_bits != MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX) ||
            state.hlod_scale_bits != MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX) ||
            state.hlod_quantize_scales != g_quantization_config.quantize_scales)) { return false; }
    const SortBufferHandles sort = subsystem_state.sorting_pipeline->get_buffer_handles();
    if (!sort.valid || sort.capacity < input->leaf_count ||
            !rd->buffer_is_valid(sort.keys_buffer) || !rd->buffer_is_valid(sort.indices_buffer)) { return false; }
    const uint32_t relative_capacity = MIN<uint32_t>(input->leaf_count, MAX(1, get_performance_settings().max_splats));
    for (bool relative : { false, true }) {
        const auto &bundle = relative ? state.hlod_relative : state.hlod_canonical;
        const auto &b = bundle.buffers;
        if (bundle.generation != uint64_t(input->get_instance_id()) || bundle.device_id != rd->get_device_instance_id() ||
                bundle.quantized != quantized || b.quantization_required != quantized || b.hlod_relative != relative ||
                b.hlod_source_generation != bundle.generation || b.instance_count != 1 ||
                b.atlas_gaussian_count != input->leaf_count + (relative ? input->parent_count : 0) ||
                b.max_visible_splats != (relative ? relative_capacity : input->leaf_count) ||
                b.max_visible_chunks != input->nodes.size() || bundle.draw_entries.size() != int(input->nodes.size()) ||
                bundle.owned_count != uint32_t(12 + int(relative) + int(quantized)) ||
                bundle.bytes != hlod_bundle_bytes(*input.ptr(), relative, quantized, b.max_visible_splats)) { return false; }
        for (uint32_t i = 0; i < bundle.owned_count; ++i) {
            if (!bundle.owned[i].is_valid() || get_resource_owner(bundle.owned[i], nullptr) != rd ||
                    !rd->buffer_is_valid(bundle.owned[i])) { return false; }
        }
        auto ready = b;
        ready.sort_key_buffer = sort.keys_buffer;
        ready.sort_value_buffer = sort.indices_buffer;
        if (!GaussianSplatting::InstancePipelineContract::has_atlas_buffers(ready) ||
                !GaussianSplatting::InstancePipelineContract::has_cull_buffers(ready) ||
                !GaussianSplatting::InstancePipelineContract::has_sort_buffers(ready) ||
                !GaussianSplatting::InstancePipelineContract::has_raster_buffers(ready)) { return false; }
    }
    const uint64_t budget = rd->get_device_memory_budget();
    const uint64_t limit = budget ? MIN(budget, GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT) : GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT;
    const uint32_t override_mb = gs::settings::get_uint(ProjectSettings::get_singleton(), "rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb", 0u);
    const uint64_t atlas_limit = ResidentAtlasBudget::compute_effective_atlas_cap_bytes(budget, override_mb);
    uint64_t bytes = 0;
    return uint64_t(input->leaf_count + input->parent_count) * (quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian)) <= atlas_limit &&
            get_hlod_owned_byte_upper_bound(bytes) && bytes <= limit;
}

bool GaussianSplatRenderer::publish_hlod_bundle(bool p_relative) {
    auto &state = get_resource_state();
    auto &bundle = p_relative ? state.hlod_relative : state.hlod_canonical;
    // This is also the only restore entry. Never bind retained A after current B
    // has fallen back, even if the caller captured its restore intent before B.
    if (!is_hlod_prepared_contract_current()) {
        if (state.hlod_prepared_snapshot.is_valid()) {
            release_hlod_resources();
            state.hlod_reason = "hlod_prepared_identity_or_readiness_changed";
        }
        return false;
    }
    if (!instance_asset_remap.valid || instance_asset_remap.generation != bundle.generation) {
        instance_asset_remap.asset_to_dense_id.clear();
        instance_asset_remap.asset_to_dense_id.insert(0, 0);
        instance_asset_remap.generation = bundle.generation;
        instance_asset_remap.valid = true;
    }
    const SortBufferHandles handles = subsystem_state.sorting_pipeline->get_buffer_handles();
    bundle.buffers.sort_key_buffer = handles.keys_buffer;
    bundle.buffers.sort_value_buffer = handles.indices_buffer;
    bundle.buffers.hlod_content_generation = ++state.hlod_content_serial;
    instance_pipeline_buffers = bundle.buffers;
    instance_backend_policy = InstanceBackendPolicy::RESIDENT;
    instance_contract_source_generation = bundle.buffers.hlod_content_generation;
    instance_contract_shape = p_relative ? "hlod_relative" : "hlod_canonical";
    instance_pipeline_buffers_valid = true;
    state.hlod_active = p_relative;
    state.instance_pipeline_content_generation = bundle.buffers.hlod_content_generation;
    return true;
}

int GaussianSplatRenderer::try_publish_hlod_resident_contract(String &r_reason) {
    auto &state = get_resource_state();
    auto *director = GaussianSplatSceneDirector::get_singleton();
    const Ref<GaussianSplatHlodRuntimeData> input = state.hlod_snapshot;
    if (state.hlod_prepared_snapshot.is_valid() && !is_hlod_prepared_contract_current()) {
        release_hlod_resources();
    }
    if (!gs::settings::get_hlod_runtime_cut_enabled(ProjectSettings::get_singleton()) || input.is_null() ||
            !director || !director->has_exclusive_hlod_world_submission(this, state.hlod_owner_key)) {
        if (state.hlod_prepared_snapshot.is_valid()) { release_hlod_resources(); }
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
        release_hlod_resources();
        return 0;
    }
    const uint32_t relative_capacity = MIN<uint32_t>(input->leaf_count,
            MAX(1, get_performance_settings().max_splats));
    const uint64_t instance_generation = director->get_instance_generation_for_renderer(this);
    const uint64_t grading_generation = state.instance_grading_defaults_generation.load(std::memory_order_relaxed);
    const uint64_t owner_key = state.hlod_owner_key;
    const uint32_t position_bits = MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX);
    const uint32_t scale_bits = MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX);
    const bool quantize_scales = g_quantization_config.quantize_scales;
    const uint32_t sort_capacity = MAX(input->leaf_count, relative_capacity);
    const SortBufferHandles existing_sort = subsystem_state.sorting_pipeline.is_valid() ? subsystem_state.sorting_pipeline->get_buffer_handles() : SortBufferHandles();
    if (
#ifdef TESTS_ENABLED
            test_hlod_publication_refuses(1) ||
#endif
            !existing_sort.valid || existing_sort.capacity < sort_capacity) {
        state.hlod_reason = "canonical_sort_capacity_warmup";
        return 0;
    }
    const bool prepared = is_hlod_prepared_contract_current();
    if (!prepared) {
        const uint64_t device_budget = rd->get_device_memory_budget();
        const uint64_t route_limit = device_budget ? MIN(device_budget, GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT) : GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT;
        const uint32_t atlas_override = gs::settings::get_uint(ProjectSettings::get_singleton(), "rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb", 0u);
        const uint64_t atlas_limit = ResidentAtlasBudget::compute_effective_atlas_cap_bytes(device_budget, atlas_override);
        if (uint64_t(input->leaf_count + input->parent_count) * (quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian)) > atlas_limit) {
            state.hlod_reason = "hlod_existing_atlas_limit"; return 0;
        }
        const uint64_t candidate_bytes = hlod_bundle_bytes(*input.ptr(), false, quantized, input->leaf_count) +
                hlod_bundle_bytes(*input.ptr(), true, quantized, relative_capacity);
        uint64_t device_bytes = 0;
        if (
#ifdef TESTS_ENABLED
                test_hlod_publication_refuses(2) ||
#endif
                !get_hlod_owned_byte_upper_bound(device_bytes)) { state.hlod_reason = "resource_ledger_warmup_or_unavailable"; return 0; }
        state.hlod_device_byte_upper_bound = device_bytes + candidate_bytes;
        if (
#ifdef TESTS_ENABLED
                test_hlod_publication_refuses(3) ||
#endif
                candidate_bytes > route_limit ||
                device_bytes > route_limit - candidate_bytes ||
                (state.hlod_prepared_snapshot.is_valid() && state.hlod_prepared_snapshot != input &&
                        input->owned_cpu_bytes > GaussianSplatHlodRuntimeData::CPU_BYTE_LIMIT - state.hlod_prepared_snapshot->owned_cpu_bytes)) {
            state.hlod_reason = "hlod_candidate_memory_limit";
            if (publish_hlod_bundle(false)) { return 1; }
            return 0;
        }
        LocalVector<InstanceDataGPU> instances;
        LocalVector<InstanceGradingGPU> grading;
        director->build_instance_buffer_for_renderer(this, instances, false);
        director->build_instance_grading_buffer_for_renderer(this, grading, false);
        if (instances.size() != 1 || grading.size() != 1) { state.hlod_reason = "hlod_identity_shim_unavailable"; return 0; }
        HlodBundle canonical, relative;
        const bool built = hlod_build_bundle(this, rd, *input.ptr(), false, quantized, input->leaf_count, position_bits, scale_bits, instances[0], grading[0], canonical) &&
                hlod_build_bundle(this, rd, *input.ptr(), true, quantized, relative_capacity, position_bits, scale_bits, instances[0], grading[0], relative);
        if (!built) {
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
        state.hlod_prepared_snapshot = input;
        state.hlod_instance_generation = instance_generation;
        state.hlod_grading_generation = grading_generation;
        state.hlod_prepared_owner_key = owner_key;
        state.hlod_position_bits = position_bits;
        state.hlod_scale_bits = scale_bits;
        state.hlod_quantize_scales = quantize_scales;
        state.hlod_device_byte_upper_bound = after_bytes;
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
    auto fallback = [&](const char *p_reason) { state.hlod_reason = p_reason; return false; };
    // Public LOD-off means the current complete canonical leaf representation.
    // Keep ordinary per-splat culling and budgets; never select coarse parents.
    if (!get_lod_enabled()) { return fallback("lod_disabled_canonical_leaves"); }
    if (p_context.pass_kind != RenderPassKind::MAIN_VIEW || !p_context.render_data || !p_context.render_buffers ||
            p_context.render_buffers->get_view_count() != 1 || p_context.projection.is_orthogonal() || p_context.painterly_enabled ||
            p_context.viewport_size.x <= 0 || p_context.viewport_size.y <= 0) {
        return fallback("unsupported_view_or_material");
    }
    ProjectSettings *ps = ProjectSettings::get_singleton();
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director || !director->has_exclusive_hlod_world_submission(this, state.hlod_owner_key) ||
            gs::settings::get_wind_settings(ps).enabled || gs::settings::get_sphere_effector_settings(ps, true).enabled ||
            director->get_sphere_effector_count_for_renderer(this) != 0) { return fallback("deformation_or_instance_domain"); }
    Ref<TileRenderer> tile = tile_renderer_state.renderer;
    if (tile.is_null() || tile->get_active_device() != get_device_state().rd) { return fallback("tile_device_unavailable"); }
    const uint64_t device_budget = get_device_state().rd->get_device_memory_budget();
    const uint64_t route_limit = device_budget ? MIN(device_budget, GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT) : GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT;
    uint64_t route_bytes = 0;
    if (!get_hlod_owned_byte_upper_bound(route_bytes)) { return fallback("resource_ledger_unavailable"); }
    state.hlod_owned_byte_upper_bound = route_bytes;
    if (route_bytes > route_limit) { return fallback("route_memory_limit"); }
    const uint32_t total = input->leaf_count + input->parent_count;
    const uint32_t capacity = state.hlod_relative.buffers.max_visible_splats;
    const PipelineFeatureSet *features = p_context.deps.pipeline_features;
    if (!features) { return fallback("current_pipeline_features_unavailable"); }
    TileRenderer::RenderParams scratch_params;
    scratch_params.viewport_size = p_context.viewport_size;
    scratch_params.total_gaussians = total;
    scratch_params.max_visible_splats = capacity;
    scratch_params.request_packed_stage_data = features->enable_packed_stage_data;
    scratch_params.request_tighter_bounds = features->enable_tighter_bounds;
    scratch_params.request_sh_amortization = features->enable_sh_amortization;
    scratch_params.sh_amortization_divisor = features->sh_amortization_divisor;
    if (!tile->prepare_hlod_scratch(scratch_params,
                route_limit - route_bytes)) { return fallback("tile_capacity_warmup_or_memory_limit"); }
    if (!get_hlod_owned_byte_upper_bound(route_bytes) || route_bytes > route_limit) { return fallback("prepared_route_memory_limit"); }
    state.hlod_owned_byte_upper_bound = route_bytes;
    const uint32_t records = tile->get_allocated_overlap_capacity();
    if (records == 0) { return fallback("overlap_capacity_unavailable"); }
    const double tau = double(state.hlod_tau_px > 0 ? state.hlod_tau_px : gs::settings::get_hlod_tau_px(ps)) * get_lod_bias();
    if (!std::isfinite(tau) || tau <= 0.0) { return fallback("invalid_tau"); }
    uint64_t tau_bits = 0;
    static_assert(sizeof(tau_bits) == sizeof(tau), "HLOD policy hash width");
    memcpy(&tau_bits, &tau, sizeof(tau));
    const uint64_t policy = _mix_generation(_mix_generation(tau_bits, capacity), records);
    auto *history = frame_context_manager.acquire_hlod_history(p_context.render_buffers, input->get_instance_id(), policy,
            input->nodes.size(), state.hlod_view_epoch);
    if (!history) { return fallback("complete_view_history_unavailable"); }
    gs_hlod_runtime::CutRequest request;
    const Transform3D camera_to_world = p_context.world_to_camera_transform.affine_inverse();
    request.camera = camera_to_world.origin;
    request.frustum_enabled = get_frustum_culling();
    for (uint32_t i = 0; i < 6; ++i) {
        request.frustum[i] = camera_to_world.xform(p_context.cull_projection.get_projection_plane(Projection::Planes(i)));
    }
    request.focal_pixels = std::abs(double(p_context.cull_projection.columns[1][1])) * p_context.viewport_size.y * 0.5;
    request.tau_pixels = tau;
    request.record_budget = double(records);
    request.tile_area = double(tile->get_tile_size()) * tile->get_tile_size();
    request.splat_budget = capacity;
    request.node_budget = state.hlod_relative.buffers.max_visible_chunks;
    const uint64_t start = OS::get_singleton()->get_ticks_usec();
    const auto selected = history->select(*input.ptr(), request);
    state.hlod_cpu_cut_usec = OS::get_singleton()->get_ticks_usec() - start;
    if (selected.status == gs_hlod_runtime::CutHistory::INVALID || selected.status == gs_hlod_runtime::CutHistory::UNAVAILABLE) {
        return fallback("cut_unavailable");
    }
    auto &bundle = state.hlod_relative;
    for (uint32_t i = 0; i < selected.selected_count; ++i) {
        const uint32_t node = history->selected_node(i);
        bundle.draw_entries.write[i] = VisibleChunkRefGPU{ 0u, node };
        state.hlod_selected_parents += !input->nodes[node].topology.is_leaf() ? 1u : 0u;
    }
    RenderingDevice *rd = get_device_state().rd;
    // Upload to the relative candidate only; no canonical RID is modified.
    const uint32_t counters[3] = { selected.selected_count, 0u, 0u };
    if ((selected.selected_count && rd->buffer_update(bundle.buffers.visible_chunk_buffer, 0,
                selected.selected_count * sizeof(VisibleChunkRefGPU), bundle.draw_entries.ptr()) != OK) ||
            rd->buffer_update(bundle.buffers.counter_buffer, 0, sizeof(counters), counters) != OK) {
        return fallback("draw_list_upload_failed");
    }
    bundle.buffers.hlod_draw_count = selected.selected_count;
    state.hlod_selected_nodes = selected.selected_count;
    state.hlod_selected_splats = selected.selected_splats;
    state.hlod_estimated_records = selected.estimated_records;
    state.hlod_visited_nodes = selected.visited_nodes;
    state.hlod_budget_bound_frames += selected.budget_bound ? 1u : 0u;
    state.hlod_root_dropped_frames += selected.status == gs_hlod_runtime::CutHistory::ROOT_DROPPED ? 1u : 0u;
    if (selected.status == gs_hlod_runtime::CutHistory::ROOT_DROPPED) {
        WARN_PRINT_ONCE("HLOD root exceeds an admission cap; the complete instance is omitted. See hlod_root_dropped_frames and hlod_reason.");
    }
    if (!publish_hlod_bundle(true)) { return false; }
    state.hlod_frame_active = true;
    state.hlod_reason = selected.status == gs_hlod_runtime::CutHistory::ROOT_DROPPED ? "root_over_budget_whole_instance_dropped" : "active";
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
    if (!compositor || compositor->hlod_scratch_device == 0 || !compositor->hlod_scratch_ledger_valid ||
            (compositor->hlod_scratch_device != 0 && compositor->hlod_scratch_device != rd->get_device_instance_id())) { return false; }
    if (compositor->hlod_scratch_texture_bytes > UINT64_MAX - textures) { return false; }
    textures += compositor->hlod_scratch_texture_bytes;
    const uint64_t buffers = rd->get_memory_usage(RD::MEMORY_BUFFERS);
    if (textures > UINT64_MAX - buffers) { return false; }
    r_bytes = buffers + textures;
    return true;
}

#ifdef TESTS_ENABLED
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

Dictionary GaussianSplatRenderer::test_observe_hlod_publication() {
    Dictionary result;
    // Explicit test readback on the actual render thread, never a production
    // per-frame readback or a fabricated successful observation. Bounded fixture.
    RenderingServer *server = RenderingServer::get_singleton();
    if (!server || !server->is_on_render_thread()) { result["error"] = "render_thread_required"; return result; }
    const auto &state = get_resource_state();
    const auto &bound = get_instance_pipeline_buffers();
    RenderingDevice *rd = get_device_state().rd;
    const Ref<GaussianData> source = get_scene_state().gaussian_data;
    if (!rd || source.is_null() || source->get_count() == 0 || source->get_count() > 64 ||
            bound.hlod_relative || bound.quantization_required || !bound.atlas_gaussian_buffer.is_valid() ||
            !rd->buffer_is_valid(bound.atlas_gaussian_buffer)) {
        result["error"] = "current_small_raw_canonical_fixture_required";
        return result;
    }
    GaussianData::SaveSnapshot snapshot;
    if (source->capture_save_snapshot(snapshot) != OK) { result["error"] = "source_snapshot_failed"; return result; }
    Vector<uint8_t> expected;
    const uint32_t count = snapshot.get_gaussians().size();
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
    const Vector<uint8_t> observed = rd->buffer_get_data(bound.atlas_gaussian_buffer);
    result["atlas_hex"] = String::hex_encode_buffer(observed.ptr(), observed.size());
    result["source_leaf_hex"] = String::hex_encode_buffer(expected.ptr(), expected.size());
    result["engine_frame"] = int64_t(Engine::get_singleton()->get_frames_drawn());
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
    director->build_instance_buffer_for_renderer(this, instances, false);
    director->build_instance_grading_buffer_for_renderer(this, grading, false);
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
    const Vector<uint8_t> instance_bytes = rd->buffer_get_data(bound.instance_buffer);
    const Vector<uint8_t> grading_bytes = rd->buffer_get_data(bound.instance_grading_buffer);
    result["instance_hex"] = String::hex_encode_buffer(instance_bytes.ptr(), instance_bytes.size());
    result["expected_instance_hex"] = String::hex_encode_buffer(reinterpret_cast<const uint8_t *>(instances.ptr()), instances.size() * sizeof(InstanceDataGPU));
    result["grading_hex"] = String::hex_encode_buffer(grading_bytes.ptr(), grading_bytes.size());
    result["expected_grading_hex"] = String::hex_encode_buffer(reinterpret_cast<const uint8_t *>(grading.ptr()), grading.size() * sizeof(InstanceGradingGPU));
    result["current_instance_generation"] = int64_t(director->get_instance_generation_for_renderer(this));
    result["current_grading_generation"] = int64_t(state.instance_grading_defaults_generation.load(std::memory_order_relaxed));
    result["relative_capacity"] = MIN<uint32_t>(count, MAX(1, get_performance_settings().max_splats));
    result["stats"] = get_render_stats();
    result["rendered_content"] = has_rendered_content();
    return result;
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
